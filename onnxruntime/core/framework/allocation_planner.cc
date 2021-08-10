// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "core/framework/allocation_planner.h"
#include <list>
#include <unordered_map>
#include <algorithm>
#include <sstream>
#include "core/common/exceptions.h"
#include "core/platform/env.h"
#include "core/framework/data_types.h"
#include "core/framework/kernel_def_builder.h"
#include "core/framework/mldata_type_utils.h"
#include "core/framework/op_kernel.h"
#include "core/framework/session_state.h"
#include "core/framework/tensorprotoutils.h"
#include "core/framework/utils.h"

using namespace onnxruntime::common;
using namespace ONNX_NAMESPACE;
namespace onnxruntime {

std::ostream& operator<<(std::ostream& out, AllocKind alloc_kind) {
  switch (alloc_kind) {
    case AllocKind::kAllocate:
      out << "Allocate";
      break;
    case AllocKind::kAllocateStatically:
      out << "AllocateStatically";
      break;
    case AllocKind::kPreExisting:
      out << "PreExisting";
      break;
    case AllocKind::kReuse:
      out << "Reuse";
      break;
    case AllocKind::kAllocateOutput:
      out << "AllocateOutput";
      break;
    case AllocKind::kShare:
      out << "Share";
      break;
    case AllocKind::kAllocatedExternally:
      out << "AllocatedExternally";
      break;
    case AllocKind::kNotSet:
      out << "NotSet";
      break;
  }
  return out;
}

// Output details of an execution plan:
std::ostream& operator<<(std::ostream& out, std::pair<const SequentialExecutionPlan*, const SessionState*> planinfo) {
  const SequentialExecutionPlan& plan = *planinfo.first;
  const SessionState& session_state = *planinfo.second;
  auto& graph = session_state.GetGraphViewer();
  std::unordered_map<int, std::string> index_to_name;

  out << "Allocation Plan:\n";
  out << "(ort_value_idx) output_name : <allocation plan>\n";
  auto plan_size = plan.allocation_plan.size();

  for (auto& name_index : session_state.GetOrtValueNameIdxMap()) {
    auto index = name_index.second;
    index_to_name[index] = name_index.first;
    out << "(" << index << ") " << name_index.first << " : ";
    if (0 <= index && static_cast<size_t>(index) < plan_size) {
      auto& elt_plan = plan.allocation_plan[index];
      out << elt_plan.alloc_kind;
      if (elt_plan.alloc_kind == AllocKind::kReuse) out << " " << elt_plan.reused_buffer;

      auto& loc = elt_plan.location;
      out << ", " << loc.ToString();

      if (elt_plan.create_fence_if_async) out << ", use fence when async";

    } else {
      out << "Index out-of-range!";
    }

    out << std::endl;
  }

  out << "\nExecution Plan:\n";
  for (size_t i = 0; i < plan.execution_plan.size(); ++i) {
    auto& step = plan.execution_plan[i];
    auto node = graph.GetNode(step.node_index);
    ORT_ENFORCE(nullptr != node);
    out << "[" << i << "] ";
    out << node->OpType() << " (" << node->Name() << ")" << std::endl;
    if (step.free_from_index <= step.free_to_index) {
      out << "Free ml-values: ";
      std::string sep;
      for (int j = step.free_from_index; j <= step.free_to_index; ++j) {
        auto freed_value_index = plan.to_be_freed[j];
        auto name_iter = index_to_name.find(freed_value_index);
        auto name = (name_iter == index_to_name.end()) ? "INVALID INDEX" : name_iter->second;
        out << sep << "(" << freed_value_index << ") " << name;
        sep = ", ";
      }
      out << std::endl;
    }
  }

  return out;
}

static const KernelCreateInfo& GetKernelCreateInfo(
    const KernelCreateInfoMap& kernel_create_info_map,
    NodeIndex node_index) {
  auto entry = kernel_create_info_map.find(node_index);
  ORT_ENFORCE(entry != kernel_create_info_map.cend(),
              "SessionState should have saved the KernelCreateInfo prior to this running. NodeIndex:", node_index);

  return *entry->second;
}

class PlannerImpl {
 public:
  PlannerImpl(const Node* parent_node, const onnxruntime::GraphViewer& graph_viewer,
              const std::vector<const NodeArg*>& outer_scope_node_args, const ExecutionProviders& providers,
              const KernelCreateInfoMap& kernel_create_info_map,
              const SubgraphsKernelCreateInfoMaps& subgraphs_kernel_create_info_maps,
              const std::unordered_map<std::string, std::reference_wrapper<const ExecutionProviders>>& subgraphs_execution_providers,
              const OrtValueNameIdxMap& ort_value_name_idx_map,
              const ISequentialPlannerContext& context, SequentialExecutionPlan& plan)
      : context_(context),
        plan_(plan),
        parent_node_(parent_node),
        graph_viewer_(graph_viewer),
        outer_scope_node_args_(outer_scope_node_args),
        execution_providers_(providers),
        kernel_create_info_map_(kernel_create_info_map),
        subgraphs_kernel_create_info_maps_(subgraphs_kernel_create_info_maps),
        subgraphs_execution_providers_(subgraphs_execution_providers),
        ort_value_name_idx_map_(ort_value_name_idx_map) {}

  Status CreatePlan();

 private:
  const ISequentialPlannerContext& context_;
  SequentialExecutionPlan& plan_;

  const Node* parent_node_;
  const onnxruntime::GraphViewer& graph_viewer_;
  const std::vector<const NodeArg*>& outer_scope_node_args_;
  const ExecutionProviders& execution_providers_;

  const KernelCreateInfoMap& kernel_create_info_map_;
  const SubgraphsKernelCreateInfoMaps& subgraphs_kernel_create_info_maps_;

  const std::unordered_map<std::string, std::reference_wrapper<const ExecutionProviders>>& subgraphs_execution_providers_;

  const OrtValueNameIdxMap& ort_value_name_idx_map_;

  // OrtValueInfo: Auxiliary information about an OrtValue used only during plan-generation:
  struct OrtValueInfo {
    const onnxruntime::NodeArg* p_def_site;  // the (unique) NodeArg corresponding to the MLValue
    int usecount = 0;                        // static reference-count

    // This is initialized to -1 to ensure that if ProcessDef is somehow not called, planning
    // will fail more cleanly.  This is also used as a temporary workaround to detect the
    // case that the DML provider has removed initilizers from the graph during partitioning.
    // Removing initializers is a temporary measure needed to limit the number of copies of
    // tensors in GPU memory.
    OrtValueIndex reused_buffer_index = -1;  // index of original buffer to reuse
#if !defined(ORT_MINIMAL_BUILD) && defined(ORT_MEMORY_PROFILE)
    OrtValueIndex inplace_reused_buffer_index = -1;  // index of original buffer to reuse inplace
#endif
  };

  // ort_value_info_ is indexed by an OrtValueIndex
  std::vector<OrtValueInfo> ort_value_info_;

  // FreeBufferInfo is used to track information about ml-values whose buffers are
  // free to be reused.
  struct FreeBufferInfo {
    OrtValueIndex ml_value;
    // deallocate_point is an index into the execution-plan; thus, ml_value becomes free after
    // this step in the execution-plan is completed.
    size_t deallocate_point;
    FreeBufferInfo(OrtValueIndex ort_value, size_t dealloc_point)
        : ml_value(ort_value), deallocate_point(dealloc_point) {}
  };
  // freelist_ : a list of ml-values whose buffers are free to be reused, sorted by when
  // they became free (more recently freed earlier in the list).
  std::list<FreeBufferInfo> freelist_;

  OrtValueIndex Index(const OrtValueName& name) {
    OrtValueIndex result;
    auto status = ort_value_name_idx_map_.GetIdx(name, result);
    ORT_ENFORCE(status.IsOK(), status.ErrorMessage());
    return result;
  }

  int& UseCount(OrtValueIndex n) {
    ORT_ENFORCE(n >= 0 && static_cast<size_t>(n) < ort_value_info_.size());
    return ort_value_info_[n].usecount;
  }
  int& UseCount(const OrtValueName& name) { return UseCount(Index(name)); }

  int DecrementUseCount(OrtValueIndex n) {
    int& use_count = --UseCount(n);
    assert(use_count >= 0);
    return use_count;
  }

#if !defined(ORT_MINIMAL_BUILD) && defined(ORT_MEMORY_PROFILE)
  OrtValueIndex& InplaceBuffer(OrtValueIndex n) {
    ORT_ENFORCE(n >= 0 && static_cast<size_t>(n) < ort_value_info_.size());
    return ort_value_info_[n].inplace_reused_buffer_index;
  }
#endif

  OrtValueIndex& Buffer(OrtValueIndex n) {
    ORT_ENFORCE(n >= 0 && static_cast<size_t>(n) < ort_value_info_.size());
    return ort_value_info_[n].reused_buffer_index;
  }

  AllocPlanPerValue& AllocPlan(OrtValueIndex n) {
    ORT_ENFORCE(n >= 0 && static_cast<size_t>(n) < plan_.allocation_plan.size());
    return plan_.allocation_plan[static_cast<size_t>(n)];
  }

  AllocPlanPerValue& AllocPlan(const OrtValueName& name) { return AllocPlan(Index(name)); }

  // Initialize state for a given ml-value at its definition site:
  void ProcessDef(OrtValueIndex id, const onnxruntime::NodeArg* p_def_site) {
    ORT_ENFORCE(id >= 0 && static_cast<size_t>(id) < ort_value_info_.size());
    OrtValueInfo& info = ort_value_info_[id];
    info.usecount = 0;
    info.reused_buffer_index = id;  // initially, no reuse; the ml-value uses its own buffer
#if !defined(ORT_MINIMAL_BUILD) && defined(ORT_MEMORY_PROFILE)
    info.inplace_reused_buffer_index = id;  // initially, no reuse; the ml-value uses its own buffer
#endif

    info.p_def_site = p_def_site;
  }

  // Reuse/Alias/Share between two OrtValue indexes
  void Reuse(OrtValueIndex reused, OrtValueIndex reused_for, AllocKind alloc_kind) {
    ORT_ENFORCE(reused != reused_for);
    // find original buffer underlying ml-value we want to reuse:
    OrtValueIndex original = Buffer(reused);
    // record that the new buffer will reuse that original buffer
    Buffer(reused_for) = original;
    // adjust original buffer's usecount
    UseCount(original) += UseCount(reused_for);

    // update allocation plan (for use at execution-time)
    auto& symplan = AllocPlan(reused_for);
    symplan.alloc_kind = alloc_kind;
    symplan.reused_buffer = original;
  }

#if !defined(ORT_MINIMAL_BUILD) && defined(ORT_MEMORY_PROFILE)
  void InplaceReuse(OrtValueIndex reused, OrtValueIndex reused_for) {
    ORT_ENFORCE(reused != reused_for);
    OrtValueIndex original = InplaceBuffer(reused);
    InplaceBuffer(reused_for) = original;
    AllocPlan(reused_for).inplace_reuse = original;
  }
#endif

  // Find if there exists some input tensor that we can use in-place for output_arg_num-th input in the node.
  bool FindReusableInput(const onnxruntime::Node& node, int output_arg_num, OrtValueIndex* reusable_input) {
#ifdef ENABLE_TRAINING
    // Inputs of Yields are essentially the outputs for FW partial subgraph
    // Thses tensors will be pass back to pytorch, thus cannot share the buffer with other tensors

    // Unhandled corner case:
    // If FW output tensor is consumed by BW graph, and pytorch performs an inplace operation on th returned tensor,
    // we will run into a buffer corruption problem.
    // One potential fix is returning a copy of output tensor, if it has downstream dependency
    auto p_next_node = node.OutputNodesBegin();
    if (p_next_node != node.OutputNodesEnd() && p_next_node->OpType() == "YieldOp") {
      return false;
    }
#endif  //ENABLE_TRAINING

    auto p_output_arg = node.OutputDefs()[output_arg_num];
    const KernelCreateInfo& ci = GetKernelCreateInfo(kernel_create_info_map_, node.Index());

    if (ci.kernel_def == nullptr) {
      return false;
    }

    const std::vector<std::pair<int, int>>& alias_map = ci.kernel_def->Alias();
    auto input_args = node.InputDefs();
    for (auto pair : alias_map) {
      if (pair.second == output_arg_num) {
        // we _must_ reuse this input to satisfy aliasing requirement: (e.g., for reshape)
        if ((0 <= pair.first) && (static_cast<size_t>(pair.first) < input_args.size())) {
          auto p_input_arg = input_args[pair.first];
          if (p_input_arg->Exists()) {
            *reusable_input = Index(p_input_arg->Name());
            return true;
          }
        }
      }
    }

    const optional<std::pair<int, int>>& variadic_alias_offsets = ci.kernel_def->VariadicAlias();
    if (variadic_alias_offsets.has_value()) {
      int input_offset = variadic_alias_offsets.value().first;
      int output_offset = variadic_alias_offsets.value().second;
      // we _must_ reuse this input to satisfy aliasing requirement: (e.g., for AllReduce)
      int alias_input_index = output_arg_num - output_offset + input_offset;
      if (alias_input_index >= 0 && static_cast<size_t>(alias_input_index) < input_args.size()) {
        auto p_input_arg = input_args[alias_input_index];
        if (p_input_arg->Exists()) {
          *reusable_input = Index(p_input_arg->Name());
          return true;
        }
      }
    }

    const std::vector<std::pair<int, int>>& inplace_map = ci.kernel_def->MayInplace();
    for (auto pair : inplace_map) {
      if (pair.second == output_arg_num) {
        if ((0 <= pair.first) && (static_cast<size_t>(pair.first) < input_args.size())) {
          auto p_input_arg = input_args[pair.first];
          if (p_input_arg->Exists()) {
            auto input_arg_index = Index(p_input_arg->Name());
            auto original = Buffer(input_arg_index);
            if (1 == UseCount(original)) {
              if (SameSize(*p_input_arg, *p_output_arg)) {
                // we can reuse this input since it is its last use and permitted for in-place update
                *reusable_input = input_arg_index;  // or original; both should be okay
                return true;
              }
            }
          }
        }
      }
    }
    return false;
  }

  static bool SameShape(const TensorShapeProto& shape1, const TensorShapeProto& shape2) {
    // TODO: This should probably be defined to be the equality operator on TensorShapeProto.
    namespace on = ONNX_NAMESPACE;
    int rank1 = shape1.dim_size();
    if (shape2.dim_size() != rank1) return false;
    for (int i = 0; i < rank1; i++) {
      const auto& val1 = shape1.dim(i);
      const auto& val2 = shape2.dim(i);
      if (utils::HasDimValue(val1) && utils::HasDimValue(val2) &&
          (val1.dim_value() == val2.dim_value()))
        continue;  // same known dimension
      if (utils::HasDimParam(val1) && utils::HasDimParam(val2)) {
        const auto& val1_param = val1.dim_param();
        if (val1_param == val2.dim_param() && !val1_param.empty())
          continue;  // same unknown dimension
      }
      return false;
    }
    return true;
  }

  /*! \brief Given a tensor-type, return the size of an element of the tensor.
  */
  static size_t GetElementSize(const DataType& tensor_type) {
    const TypeProto& type_proto = ONNX_NAMESPACE::Utils::DataTypeUtils::ToTypeProto(tensor_type);
    MLDataType ml_data_type = DataTypeImpl::TypeFromProto(type_proto);
    const TensorTypeBase* tensor_type_base = ml_data_type->AsTensorType();
    ORT_ENFORCE(nullptr != tensor_type_base);
    MLDataType elt_type = tensor_type_base->GetElementType();
    return elt_type->Size();
  }

  static bool SameSize(const TensorShapeProto& shape1, const onnxruntime::NodeArg& arg1,
                       const TensorShapeProto& shape2, const onnxruntime::NodeArg& arg2) {
    const auto& ptype1 = arg1.Type();
    const auto& ptype2 = arg2.Type();
    auto type1_size = GetElementSize(ptype1);
    auto type2_size = GetElementSize(ptype2);
    bool is_type1_string = arg1.TypeAsProto()->tensor_type().elem_type() == ONNX_NAMESPACE::TensorProto_DataType_STRING;
    bool is_type2_string = arg2.TypeAsProto()->tensor_type().elem_type() == ONNX_NAMESPACE::TensorProto_DataType_STRING;

    // sizeof(std::string) = sizeof(double) on gcc 4.8.x on CentOS. This causes the allocation planner to reuse
    // a tensor of type double. This won't work for string tensors since they need to be placement new'ed.
    // If either of the tensors is a string, don't treat them the same. Moreover, reusing a string tensor for a string
    // tensor without releasing the previous memory can cause memory leaks; hence we don't allow reuse across string
    // tensors as well.
    return !(is_type1_string || is_type2_string) && (type1_size == type2_size) && SameShape(shape1, shape2);

    /* TODO: we can improve this if the concrete shapes are known for both as below.
       Unclear whether this is worthwhile though.
    if (KnownSize(p_shape1) && KnownSize(p_shape2)) {
      // Comparison of statically-known size
      auto size1 = NumElements(p_shape1) * EltSize(ptype1);
      auto size2 = NumElements(p_shape2) * EltSize(ptype2);
      return size1 == size2;
    } else {
      // Comparison of statically-unknown size buffers
      return SameElementSize(ptype1, ptype2) && SameShape(shape1, shape2);
    }
    */
  }

  bool SameSize(const onnxruntime::NodeArg& arg1, const onnxruntime::NodeArg& arg2) {
    if ((!arg1.Exists()) || (!arg2.Exists())) return false;
    auto p_shape1 = context_.GetShape(arg1);
    auto p_shape2 = context_.GetShape(arg2);
    // If the shapes are unknown, we conservatively assume they may be of different size.
    if ((nullptr == p_shape1) || (nullptr == p_shape2)) return false;
    return SameSize(*p_shape1, arg1, *p_shape2, arg2);
  }

  // Find if freelist contains a buffer of the same size as output_arg
  bool FindReusableTensor(const onnxruntime::NodeArg& output_arg, OrtValueIndex* reusable_tensor) {
    if (!context_.GetEnableMemoryReuse()) {
      return false;
    }
    auto p_required_buffer_shape = context_.GetShape(output_arg);
    if (nullptr == p_required_buffer_shape || p_required_buffer_shape->dim_size() == 0) return false;
    auto& required_memory_info = AllocPlan(output_arg.Name()).location;
    if (HasFence(&output_arg)) return false;

    for (auto it = freelist_.begin(); it != freelist_.end(); ++it) {
      size_t reusable = static_cast<size_t>(it->ml_value);
      const onnxruntime::NodeArg* p_node_arg = ort_value_info_.at(reusable).p_def_site;
      if (!p_node_arg) {
        // TODO this should be an error case, needs more investigation
        continue;
      }
      auto& available_memory_info = AllocPlan(p_node_arg->Name()).location;
      if (!(available_memory_info == required_memory_info)) continue;
      auto p_available_buffer_shape = context_.GetShape(*p_node_arg);
      if (nullptr != p_available_buffer_shape) {
        if (SameSize(*p_available_buffer_shape, *p_node_arg,
                     *p_required_buffer_shape, output_arg)) {
          *reusable_tensor = it->ml_value;
          freelist_.erase(it);
          return true;
        }
      }
    }
    return false;
  }

  void Initialize(size_t num_graph_nodes, size_t num_ml_values) {
    // All ml-value indices must be in range 0 .. num_ml_values-1
    ort_value_info_.resize(num_ml_values);

    // Initialize execution plan:
    plan_.execution_plan.reserve(num_graph_nodes);

    // Initialize node_has_fence.
    plan_.node_has_fence.resize(graph_viewer_.MaxNodeIndex());

    // Initialize allocation plan:
    plan_.allocation_plan.resize(num_ml_values);
  }

  bool HasExternalOutputs(const Node& node) const {
    const KernelCreateInfo& ci = GetKernelCreateInfo(kernel_create_info_map_, node.Index());
    if (ci.kernel_def == nullptr) {
      return false;
    }

    return ci.kernel_def->HasExternalOutputs();
  }

  Status ImplicitInputLocationHelper(const GraphViewer& graph_viewer,
                                     const std::string& implicit_input,
                                     const std::string& subgraph_kernel_create_info_map_key_base,
                                     size_t graph_depth,
                                     size_t parent_node_index,
                                     const std::string& subgraph_attr_name,
                                     /*out*/ const OrtMemoryInfo*& memory_info) {
    std::ostringstream ss;

    // key = base + depth + current graph node index + attr name corresponding to the subgraph
    ss << subgraph_kernel_create_info_map_key_base;
    ss << graph_depth;
    ss << parent_node_index;
    ss << subgraph_attr_name;

    const auto& local_subgraph_kernel_create_info_map_key = ss.str();

    auto specific_subgraph_map_for_node = subgraphs_kernel_create_info_maps_.find(local_subgraph_kernel_create_info_map_key);
    ORT_ENFORCE(specific_subgraph_map_for_node != subgraphs_kernel_create_info_maps_.end());

    auto specific_subgraph_execution_providers = subgraphs_execution_providers_.find(local_subgraph_kernel_create_info_map_key);
    ORT_ENFORCE(specific_subgraph_execution_providers != subgraphs_execution_providers_.end());

    for (const auto& node : graph_viewer.Nodes()) {
      const auto& input_node_args = node.InputDefs();
      size_t num_node_inputs = input_node_args.size();

      for (size_t node_input_index = 0; node_input_index < num_node_inputs; ++node_input_index) {
        auto input_node_arg = input_node_args[node_input_index];

        // Skip processing missing optional inputs
        if (!input_node_arg->Exists()) {
          continue;
        }

        ORT_TRY {
          auto& def_name = input_node_arg->Name();

          if (def_name == implicit_input) {
            const auto& kernel_create_info = GetKernelCreateInfo(specific_subgraph_map_for_node->second, node.Index());
            const auto* p_kernel_def = kernel_create_info.kernel_def.get();
            ORT_ENFORCE(p_kernel_def, "Should not have entry in kernel create info with nullptr for kernel_def");

            const auto* exec_provider = specific_subgraph_execution_providers->second.get().Get(node);
            ORT_ENFORCE(exec_provider != nullptr,
                        "Can not find the execution provider ", node.GetExecutionProviderType());

            // Assign the found output
            OrtMemType mem_type = p_kernel_def->InputMemoryType(node_input_index);
            memory_info = &exec_provider->GetAllocator(0, mem_type)->Info();

            return Status::OK();
          }
        }
        ORT_CATCH(const std::exception& ex) {
          ORT_HANDLE_EXCEPTION([&]() {
            return ORT_MAKE_STATUS(ONNXRUNTIME, FAIL, ex.what());
          });
        }
      }

      // If the node has subgraphs (i.e.) control flow nodes,
      // walk the nodes in those subgraphs as well to best determine
      // the location for the OrtValue corresponding to the weights
      // (i.e.) do a recursion
      if (node.ContainsSubgraph()) {
        for (auto& name_to_subgraph : node.GetAttributeNameToSubgraphMap()) {
          GraphViewer subgraph_viewer(*name_to_subgraph.second);

          ORT_RETURN_IF_ERROR(ImplicitInputLocationHelper(subgraph_viewer,
                                                          implicit_input,
                                                          local_subgraph_kernel_create_info_map_key,
                                                          graph_depth + 1,
                                                          node.Index(),
                                                          name_to_subgraph.first,
                                                          memory_info));

          // We have found the location - no need to traverse more subgraphs
          if (memory_info != nullptr) {
            return Status::OK();
          }
        }
      }
    }

    return Status::OK();
  }

  Status ComputeUseCounts() {
    // Note: for every ml-value, its definition must appear before all its uses in a topological sort of a valid model
    std::unordered_set<std::string> graph_inputs;
    for (auto& graph_input : graph_viewer_.GetInputsIncludingInitializers()) {
      graph_inputs.insert(graph_input->Name());
    }

    for (auto graph_input : graph_viewer_.GetInputs()) {
      OrtValueIndex index = Index(graph_input->Name());
      ProcessDef(index, graph_input);
      UseCount(index)++;  // Models caller's usage post-inference; ensures it will not be reused.
    }

    for (auto node_arg : outer_scope_node_args_) {
      OrtValueIndex index = Index(node_arg->Name());
      ProcessDef(index, node_arg);
      UseCount(index)++;  // ensure will not be re-used as this graph does not own the buffer
    }

    // All initializers should be treated as input
    for (const auto& pair : graph_viewer_.GetAllInitializedTensors()) {
      const auto& initializer_name = pair.first;
      OrtValueIndex index = Index(initializer_name);
      ProcessDef(index, graph_viewer_.GetNodeArg(pair.first));
      UseCount(initializer_name)++;
    }

    for (SequentialExecutionPlan::NodeExecutionPlan& step : plan_.execution_plan) {
      auto pnode = graph_viewer_.GetNode(step.node_index);
      if (pnode == nullptr) {
        return ORT_MAKE_STATUS(ONNXRUNTIME, FAIL, "Can not find the node ", step.node_index);
      }

      // Identify where each output of this node should be allocated.
      // This is determined by the OpKernel bound to the node.
      const KernelCreateInfo& kernel_create_info = GetKernelCreateInfo(kernel_create_info_map_, pnode->Index());

      const auto* p_kernel_def = kernel_create_info.kernel_def.get();

      ORT_ENFORCE(p_kernel_def, "Should not have entry in kernel create info with nullptr for kernel_def");

      auto exec_provider = execution_providers_.Get(*pnode);
      if (exec_provider == nullptr) {
        return ORT_MAKE_STATUS(ONNXRUNTIME, FAIL, "Can not find the execution provider ",
                               pnode->GetExecutionProviderType());
      }

      bool is_implicit_input = false;

      // increment UseCount and add location information if applicable for the provided input def
      auto process_input = [&graph_inputs, &exec_provider, &p_kernel_def, &is_implicit_input, &pnode,
                            this](const NodeArg& input, size_t arg_idx) {
        const auto& name = input.Name();
        UseCount(name)++;

        // If it's a graph input or outer scope node arg, set its plan.
        // NOTE: Copy nodes should have already been added if a graph input is fed as input
        // to nodes assigned to different providers.
        if (graph_inputs.find(name) != graph_inputs.cend() ||
            std::find_if(outer_scope_node_args_.cbegin(), outer_scope_node_args_.cend(),
                         [&name](const NodeArg* value) {
                           return value && value->Name() == name;
                         }) != outer_scope_node_args_.cend()) {
          if (name == "1389") {
            float f = 1.2f;
            ORT_IGNORE_RETURN_VALUE(f);
          }

          OrtValueIndex index = Index(name);

          if (!is_implicit_input) {
            OrtMemType mem_type = p_kernel_def->InputMemoryType(arg_idx);
            plan_.SetLocation(static_cast<size_t>(index), exec_provider->GetAllocator(0, mem_type)->Info());
          } else {
            if (std::find_if(outer_scope_node_args_.cbegin(), outer_scope_node_args_.cend(),
                             [&name](const NodeArg* value) {
                               return value && value->Name() == name;
                             }) != outer_scope_node_args_.cend()) {
              // For implicit inputs, recurse into the subgraph(s) held by this node
              // and find its actual usage node and update the location.
              ORT_ENFORCE(pnode->ContainsSubgraph());

              const OrtMemoryInfo* memory_info = nullptr;

              for (auto& name_to_subgraph : pnode->GetAttributeNameToSubgraphMap()) {
                GraphViewer subgraph_viewer(*name_to_subgraph.second);

                ORT_RETURN_IF_ERROR(ImplicitInputLocationHelper(subgraph_viewer,
                                                                name,
                                                                "",
                                                                0,
                                                                pnode->Index(),
                                                                name_to_subgraph.first,
                                                                memory_info));

                // This subgraph uses the implicit input - no need to iterate though more subgraphs
                if (memory_info != nullptr) {
                  break;
                }
              }

              // If none of the nested subgraphs use this implicit input,
              // it point to some internal error.
              ORT_ENFORCE(memory_info != nullptr);
              plan_.SetLocation(static_cast<size_t>(index), *memory_info);
            }
          }
        }
        return Status::OK();
      };

      ORT_RETURN_IF_ERROR(Node::ForEachWithIndex(pnode->InputDefs(), process_input));

      is_implicit_input = true;
      ORT_RETURN_IF_ERROR(Node::ForEachWithIndex(pnode->ImplicitInputDefs(), process_input));

      auto outputs = pnode->OutputDefs();
      auto num_outputs = outputs.size();
      bool has_external_outputs = HasExternalOutputs(*pnode);
      for (size_t i = 0; i < num_outputs; ++i) {
        auto* node_output = outputs[i];
        if (!node_output->Exists()) continue;
        if (node_output->Name() == "1389") {
          float f = 1.2f;
          ORT_IGNORE_RETURN_VALUE(f);
        }
        OrtValueIndex index = Index(node_output->Name());
        ProcessDef(index, node_output);
        // Ensures external outputs will not be reused.
        UseCount(index) += (has_external_outputs ? 2 : 1);
        auto allocator = exec_provider->GetAllocator(0, p_kernel_def->OutputMemoryType(i));
        ORT_ENFORCE(allocator);
        plan_.SetLocation(static_cast<size_t>(index),
                          allocator->Info());
      }

      // if sync is needed, mark allocation plan as create_fence_if_async=true
      // note that the input arg may come from an execution provider (i.e. CPU) that does not support async,
      // in which case create_fence_if_async would be ignored when creating MLValue
      if (p_kernel_def->ExecQueueId() != 0) {
        pnode->ForEachDef([this](const onnxruntime::NodeArg& arg, bool /*is_input*/) {
          OrtValueIndex index = Index(arg.Name());
          AllocPlan(index).create_fence_if_async = true;
        });
      }
    }

    for (auto graph_output : graph_viewer_.GetOutputs()) {
      UseCount(graph_output->Name())++;  // Models caller's usage post-inference; ensures it will not be reused.
    }

    return Status::OK();
  }

  OrtMemoryInfo GetLocationForNodeInput(size_t input_index, const Node& node,
                                        const KernelCreateInfoMap& kernel_create_info_map,
                                        const ExecutionProviders& execution_providers) {
    auto* p_provider = execution_providers.Get(node);
    ORT_ENFORCE(p_provider);

    const KernelCreateInfo& kernel_create_info = GetKernelCreateInfo(kernel_create_info_map, node.Index());

    if (utils::IsInputOnCpu(node, &kernel_create_info, input_index))
      // weights are not output from any node, so it's OK to put its location on CPU provider
      return execution_providers.GetDefaultCpuMemoryInfo();
    return p_provider->GetAllocator(0, OrtMemTypeDefault)->Info();
  }

  Status GeneratePlanForWeightsHelper(const GraphViewer& graph_viewer,
                                      const InitializedTensorSet& weights,
                                      const KernelCreateInfoMap& kernel_create_info_map,
                                      const ExecutionProviders& execution_providers,
                                      const std::string& subgraph_kernel_create_info_map_key_base,
                                      size_t graph_depth,
                                      /*out*/ std::vector<std::vector<OrtMemoryInfo>>& locations) {
    for (const auto& node : graph_viewer.Nodes()) {
      const auto& input_node_args = node.InputDefs();
      size_t num_node_inputs = input_node_args.size();

      for (size_t node_input_index = 0; node_input_index < num_node_inputs; ++node_input_index) {
        auto input_node_arg = input_node_args[node_input_index];

        // Skip processing missing optional inputs
        if (!input_node_arg->Exists()) {
          continue;
        }

        ORT_TRY {
          auto& def_name = input_node_arg->Name();

          // This node input doesn't correspond to any of the weights
          if (!weights.count(def_name)) {
            continue;
          }

          if (def_name == "2629") {
            float f = 1.2f;
            ORT_IGNORE_RETURN_VALUE(f);
          }

          // Skip processing shadow values in subgraphs.
          // While processing subgraphs, if we don't see this entry in the implicit
          // inputs of the node containing the subgraph, skip processing it as it is
          // a shadow value.
          if (graph_depth > 0) {
            // We are processing a subgraph if we enter this
            const auto* parent_node = graph_viewer.ParentNode();
            ORT_ENFORCE(parent_node);

            bool is_implicit_input = false;
            for (const auto& implicit_input : parent_node->ImplicitInputDefs()) {
              if (implicit_input->Name() == def_name) {
                is_implicit_input = true;
                break;
              }
            }

            // Only proceed if it is an implicit input
            if (!is_implicit_input) {
              continue;
            }
          }

          auto wt_index = Index(def_name);
          locations[wt_index].emplace_back(GetLocationForNodeInput(node_input_index, node, kernel_create_info_map, execution_providers));
        }
        ORT_CATCH(const std::exception& ex) {
          ORT_HANDLE_EXCEPTION([&]() {
            return ORT_MAKE_STATUS(ONNXRUNTIME, FAIL, ex.what());
          });
        }
      }

      // If the node has subgraphs (i.e.) control flow nodes,
      // walk the nodes in those subgraphs as well to best determine
      // the location for the OrtValue corresponding to the weights
      // (i.e.) do a recursion
      if (node.ContainsSubgraph()) {
        for (auto& name_to_subgraph : node.GetAttributeNameToSubgraphMap()) {
          GraphViewer subgraph_viewer(*name_to_subgraph.second);

          std::ostringstream ss;

          // key = base + depth + current graph node index + attr name corresponding to the subgraph
          ss << subgraph_kernel_create_info_map_key_base;
          ss << graph_depth;
          ss << node.Index();
          ss << name_to_subgraph.first;

          const auto& local_subgraph_kernel_create_info_map_key = ss.str();

          auto specific_subgraph_map_for_node = subgraphs_kernel_create_info_maps_.find(local_subgraph_kernel_create_info_map_key);
          ORT_ENFORCE(specific_subgraph_map_for_node != subgraphs_kernel_create_info_maps_.end());

          auto specific_subgraph_execution_providers = subgraphs_execution_providers_.find(local_subgraph_kernel_create_info_map_key);
          ORT_ENFORCE(specific_subgraph_execution_providers != subgraphs_execution_providers_.end());

          ORT_RETURN_IF_ERROR(GeneratePlanForWeightsHelper(subgraph_viewer,
                                                           weights,
                                                           specific_subgraph_map_for_node->second,
                                                           specific_subgraph_execution_providers->second,
                                                           local_subgraph_kernel_create_info_map_key,
                                                           graph_depth + 1,
                                                           locations));
        }
      }
    }

    return Status::OK();
  }

  Status GeneratePlanForWeights() {
    std::vector<std::vector<OrtMemoryInfo>> locations(plan_.allocation_plan.size());

    ORT_RETURN_IF_ERROR(GeneratePlanForWeightsHelper(graph_viewer_, graph_viewer_.GetAllInitializedTensors(),
                                                     kernel_create_info_map_, execution_providers_,
                                                     "", 0, locations));

    for (size_t i = 0; i != locations.size(); ++i) {
      const std::vector<OrtMemoryInfo>& loc = locations[i];
      if (loc.empty()) continue;
      plan_.allocation_plan[i].alloc_kind = AllocKind::kAllocateStatically;
      plan_.allocation_plan[i].location = loc[0];
#if !defined(ORT_MINIMAL_BUILD) && defined(ORT_MEMORY_PROFILE)
      size_t max_pc = plan_.execution_plan.size();
      std::string node_arg_name;
      ort_value_name_idx_map_.GetName(static_cast<int>(i), node_arg_name);
      auto node_arg = graph_viewer_.GetNodeArg(node_arg_name);
      plan_.allocation_plan[i].value_type = utils::GetMLDataType(*node_arg);
      plan_.allocation_plan[i].life_interval = std::pair<size_t, size_t>(0, max_pc);
#endif
      for (size_t j = 0; j != loc.size(); ++j) {
        if (loc[j] != loc[0]) {
          // set the location to CPU
          plan_.allocation_plan[i].location = execution_providers_.GetDefaultCpuMemoryInfo();
          break;
        }
      }
    }
    return Status::OK();
  }

  // Should only be used after ProcessDef()
  Status ComputeReusePlan() {
    std::vector<SequentialExecutionPlan::NodeExecutionPlan>& execution_plan(plan_.execution_plan);
    //copy the usecounts to an vector, before computing reuse
#if !defined(ORT_MINIMAL_BUILD) && defined(ORT_MEMORY_PROFILE)
    std::vector<int> ort_value_usecount;
    for (auto ort_value_info : ort_value_info_) {
      ort_value_usecount.push_back(ort_value_info.usecount);
    }
#endif

    // Identify allocation/deallocation plan for every ml-value

    auto setup_preexisting = [this](const NodeArg* node_arg) {
      auto input_index = Index(node_arg->Name());
      AllocPlanPerValue& thisplan = AllocPlan(input_index);
      thisplan.alloc_kind = AllocKind::kPreExisting;
      thisplan.value_type = utils::GetMLDataType(*node_arg);
#if !defined(ORT_MINIMAL_BUILD) && defined(ORT_MEMORY_PROFILE)
      size_t max_pc = plan_.execution_plan.size();
      thisplan.life_interval = std::pair<size_t, size_t>(0, max_pc);
#endif
    };

    // inputs of the graph:
    // An input ml-value's data is owned by the caller (of InferenceSession::Run())
    // It must be allocated by the caller, and will not be reused during inference.
    for (auto graph_input : graph_viewer_.GetInputs()) {
      setup_preexisting(graph_input);
    }

    // outer scope node args are treated the same as graph inputs
    for (auto outer_scope_node_arg : outer_scope_node_args_) {
      setup_preexisting(outer_scope_node_arg);
    }

    // set AllocationInfo for each weight
    ORT_RETURN_IF_ERROR(GeneratePlanForWeights());

    // Cached graph outputs.
    const auto& graph_outputs = graph_viewer_.GetOutputs();
    for (size_t program_counter = 0; program_counter < execution_plan.size(); ++program_counter) {
      SequentialExecutionPlan::NodeExecutionPlan step = execution_plan[program_counter];
      // the node (aka operator) which carries the considered program (aka computation).
      const auto* pnode = graph_viewer_.GetNode(step.node_index);
      // node outputs.
      const auto& output_defs = pnode->OutputDefs();
      // External outputs flag.
      bool has_external_outputs = HasExternalOutputs(*pnode);
      // output_arg_def_index is the index of ArgDefs in pnode's output list.
      // At the i-th iteration, we build the allocation plan for the i-th
      // NodeArg in pnode's output list. Allocation plan remains untouched for
      // optional-missing outputs (aka values with empty names).
      for (size_t output_arg_def_index = 0, end = output_defs.size(); output_arg_def_index < end; ++output_arg_def_index) {
        const auto& node_output = output_defs[output_arg_def_index];
        if (!node_output->Exists()) continue;
        // OrtValue index of the considered output NodeArg.
        const auto current = Index(node_output->Name());
        AllocPlan(current).value_type = utils::GetMLDataType(*node_output);
#if !defined(ORT_MINIMAL_BUILD) && defined(ORT_MEMORY_PROFILE)
        AllocPlan(current).life_interval.first = program_counter;
#endif
        // Declare OrtValue index of the reused buffer.
        // The the OrtValue indexed by current may reuse the memory in the OrtValue indexed by reused.
        OrtValueIndex reused;
        if (has_external_outputs) {
          ORT_ENFORCE(!IsNonTensor(*node_output), "Only tensors are supported for external outputs for now.");
          AllocPlan(current).alloc_kind = AllocKind::kAllocatedExternally;
#if !defined(ORT_MINIMAL_BUILD) && defined(ORT_MEMORY_PROFILE)
          AllocPlan(current).life_interval.second = execution_plan.size();
#endif
        } else if (std::find(graph_outputs.begin(), graph_outputs.end(), node_output) != graph_outputs.end()) {
          // node_output is graph's output, so we can't reuse intermediate buffer
          AllocPlan(current).alloc_kind = AllocKind::kAllocateOutput;
#if !defined(ORT_MINIMAL_BUILD) && defined(ORT_MEMORY_PROFILE)
          AllocPlan(current).life_interval.second = execution_plan.size();
#endif

          // hacky perf optimization to not copy a pre-existing value to an output if this is a Loop subgraph and
          // the value is not being changed in the subgraph.
          //
          // this usage of a loop state variable has been seen in two scenarios. both have better alternatives now.
          // we maintain the optimization for existing models.
          //
          // 1. a loop state variable was being provided due to ONNX not supporting empty variadic inputs.
          //    a dummy loop state variable was required in this case.
          //    ONNX now supports empty variadic inputs, so a new model should not add a dummy loop state variable.
          //
          // 2. a loop state variable was being used to explicitly pass in an outer scope value to the subgraph.
          //    this sort of usage is automatically handled via implicit inputs and there's no need to add a
          //    loop state variable in order to access the outer scope value.
          if (parent_node_ && pnode->OpType() == "Identity" && parent_node_->OpType() == "Loop") {
            const NodeArg* input = pnode->InputDefs()[0];

            // first input to the Loop subgraph is the iteration number.
            bool input_is_loop_iteration_number = input == graph_viewer_.GetInputs()[0];
            if (input_is_loop_iteration_number) {
              // as the value inside the OrtValue gets changed by the Loop implementation on each iteration
              // (so it can re-use the OrtValue instance) if it is also a subgraph output it must be allocated
              // so a copy of the current value is returned, so leave alloc_kind as kAllocateOutput
            } else {
              const auto& input_name = input->Name();
              const auto input_index = Index(input_name);

              const auto& alloc_plan = AllocPlan(input_index);
              if (alloc_plan.alloc_kind == AllocKind::kPreExisting) {
                Reuse(input_index, current, AllocKind::kShare);
              }
            }
          }
        } else if (IsNonTensor(*node_output)) {
          // we do not try sharing-optimization for non-tensors
          AllocPlan(current).alloc_kind = AllocKind::kAllocate;
          AllocPlan(current).program_counter.AddStart(program_counter);
        } else if (!context_.IsParallelExecutionEnabled() &&
                   FindReusableInput(*pnode, static_cast<int>(output_arg_def_index), &reused)) {
          // Reuse one of this node's input buffers as the output buffer (for in-place update)
          Reuse(reused, current, AllocKind::kReuse);
#if !defined(ORT_MINIMAL_BUILD) && defined(ORT_MEMORY_PROFILE)
          InplaceReuse(reused, current);
#endif
        } else if (!context_.IsParallelExecutionEnabled() &&
                   FindReusableTensor(*node_output, &reused)) {
          // Reuse an available (dead) buffer for this output, this is only for sequential execution.
          Reuse(reused, current, AllocKind::kReuse);
          OrtValueIndex original = Buffer(reused);
          if (AllocPlan(original).alloc_kind == AllocKind::kAllocate) {
            AllocPlan(original).program_counter.AddStart(program_counter);
          }
        } else {
          // otherwise: allocate a new buffer for this output
          AllocPlan(current).alloc_kind = AllocKind::kAllocate;
          AllocPlan(current).program_counter.AddStart(program_counter);
        }
      }

      // determine if inputs of *pnode can be freed:
      for (auto node_input : pnode->InputDefs()) {
        if (node_input->Exists()) {
          auto& sym = node_input->Name();
          auto original = Buffer(Index(sym));
          // The index will be -1 if it's an initializer that was removed as part of a temporary workaround.
          // See comments in the OrtValueInfo definition.
#if !defined(ORT_MINIMAL_BUILD) && defined(ORT_MEMORY_PROFILE)
          // Compute lifetime
          auto current = Index(sym);
          if ((current != -1) && (0 == --ort_value_usecount[current])) {
            AllocPlan(current).life_interval.second = program_counter;
          }
#endif
          if ((original != -1) && (0 == DecrementUseCount(original))) {
            freelist_.push_front(FreeBufferInfo(original, program_counter));
            if (AllocPlan(original).alloc_kind == AllocKind::kAllocate) {
              AllocPlan(original).program_counter.AddEnd(program_counter);
            }
          }
        }
      }

      for (auto node_input : pnode->ImplicitInputDefs()) {
        if (node_input->Exists()) {
          auto& sym = node_input->Name();
          auto original = Buffer(Index(sym));
          // The index will be -1 if it's an initializer that was removed as part of a temporary workaround.
          // See comments in the OrtValueInfo definition.
#if !defined(ORT_MINIMAL_BUILD) && defined(ORT_MEMORY_PROFILE)
          // Compute lifetime
          auto current = Index(sym);
          if ((current != -1) && (0 == --ort_value_usecount[current])) {
            AllocPlan(current).life_interval.second = program_counter;
          }
#endif
          if ((original != -1) && (0 == DecrementUseCount(original))) {
            freelist_.push_front(FreeBufferInfo(original, program_counter));
            if (AllocPlan(original).alloc_kind == AllocKind::kAllocate) {
              AllocPlan(original).program_counter.AddEnd(program_counter);
            }
          }
        }
      }

      // determine if any outputs of *pnode are unused and can be freed:
      for (auto node_output : pnode->OutputDefs()) {
        if (node_output->Exists()) {
          auto& sym = node_output->Name();
          auto original = Buffer(Index(sym));
          // The index will be -1 if it's an initializer that was removed as part of a temporary workaround.
          // See comments in the OrtValueInfo definition.
#if !defined(ORT_MINIMAL_BUILD) && defined(ORT_MEMORY_PROFILE)
          auto current = Index(sym);
          if ((current != -1) && (0 == --ort_value_usecount[current])) {
            AllocPlan(current).life_interval.second = program_counter;
          }
#endif
          if (0 == DecrementUseCount(original)) {
            freelist_.push_front(FreeBufferInfo(original, program_counter));
            if (AllocPlan(original).alloc_kind == AllocKind::kAllocate) {
              AllocPlan(original).program_counter.AddEnd(program_counter);
            }
          }
        }
      }
    }
    return Status::OK();
  }
#ifdef ENABLE_TRAINING
  bool AllocateInputsContiguously(const Node& node) const {
    const KernelCreateInfo& ci = GetKernelCreateInfo(kernel_create_info_map_, node.Index());
    if (ci.kernel_def == nullptr) {
      return false;
    }

    return ci.kernel_def->AllocateInputsContiguously();
  }

  // Compute allocation order for tensors that are required to be allocated contiguously.
  Status ComputeAllocationOrder() {
    std::vector<SequentialExecutionPlan::NodeExecutionPlan>& execution_plan(plan_.execution_plan);
    std::vector<OrtValueIndex>& initializer_allocation_order(plan_.initializer_allocation_order);
    std::vector<OrtValueIndex>& activation_allocation_order(plan_.activation_allocation_order);
    for (auto& step : execution_plan) {
      const auto* pnode = graph_viewer_.GetNode(step.node_index);
      if (pnode == nullptr) return ORT_MAKE_STATUS(ONNXRUNTIME, FAIL, "Cannot find the node ", step.node_index);
      if (!AllocateInputsContiguously(*pnode)) continue;
      // This node has requested inputs be allocated contiguously.
      const auto& input_defs = pnode->InputDefs();
      onnxruntime::AllocKind input_kind = AllocKind::kAllocateStatically;
      bool set_input_kind = true;
      for (const auto& node_input : input_defs) {
        if (!node_input->Exists()) continue;
        const auto current_idx = Index(node_input->Name());
        const auto& current_plan = AllocPlan(current_idx);
        const auto actual_idx = current_plan.alloc_kind == AllocKind::kReuse ? current_plan.reused_buffer : current_idx;
        const auto& actual_plan = AllocPlan(actual_idx);
        if (set_input_kind) {
          input_kind = actual_plan.alloc_kind;
          set_input_kind = false;
        }

        if ((actual_plan.alloc_kind == AllocKind::kAllocateStatically) && (input_kind != AllocKind::kAllocateStatically))
          return ORT_MAKE_STATUS(ONNXRUNTIME, FAIL, "AllocateInputsContiguously() requires all inputs to be initializers, or all inputs to be non-initializers.");

        if (actual_plan.alloc_kind == AllocKind::kAllocateStatically) {
          if (std::find(initializer_allocation_order.begin(), initializer_allocation_order.end(), actual_idx) == initializer_allocation_order.end())
            initializer_allocation_order.push_back(actual_idx);
        } else {
          if (std::find(activation_allocation_order.begin(), activation_allocation_order.end(), actual_idx) == activation_allocation_order.end())
            activation_allocation_order.push_back(actual_idx);
        }
      }
    }
    return Status::OK();
  }
#endif

  void VerifyMemoryTimeSchedule() {
    size_t idx = 0;
    for (const auto& entry : plan_.allocation_plan) {
      if (entry.alloc_kind == AllocKind::kAllocate) {
        ORT_ENFORCE(entry.program_counter.HasValidEntries(), "Invalid program_counter entries at index ", idx);
      }

      ++idx;
    }
  }

  // Whether a given NodeArg has fence or not.
  // If the buffer is reused, need to check whether original OrtValue has fence or not.
  bool HasFence(const onnxruntime::NodeArg* arg) {
    bool has_fence = false;
    if (arg && arg->Exists()) {
      OrtValueIndex index = Index(arg->Name());
      AllocPlanPerValue& value_plan = AllocPlan(index);

      has_fence = value_plan.create_fence_if_async;
      if (value_plan.alloc_kind == AllocKind::kReuse) {
        // Buffer reused, check original buffer to see if fence is shared.
        has_fence = has_fence || AllocPlan(value_plan.reused_buffer).create_fence_if_async;
      }
    }

    return has_fence;
  }

  // Compute fence check. Set has_fence flag if either one of inputs, implicit inputs or outputs of a given node has fence.
  Status ComputeFenceCheck() {
    for (SequentialExecutionPlan::NodeExecutionPlan& step : plan_.execution_plan) {
      auto pnode = graph_viewer_.GetNode(step.node_index);
      if (pnode == nullptr) return ORT_MAKE_STATUS(ONNXRUNTIME, FAIL, "Can not find the node ", step.node_index);

      bool has_fence = false;
      for (auto node_input : pnode->InputDefs()) {
        has_fence = has_fence || HasFence(node_input);
      }

      for (auto node_input : pnode->ImplicitInputDefs()) {
        has_fence = has_fence || HasFence(node_input);
      }

      for (auto node_output : pnode->OutputDefs()) {
        has_fence = has_fence || HasFence(node_output);
      }

      plan_.node_has_fence[step.node_index] = has_fence;
    }

    return Status::OK();
  }

  // Convert information in a freelist (about which ml-value becomes free when) into
  // a deallocation plan in the format required in an ExecutionPlan
  void GenerateDeallocationPlan() {
    // Store (indices of) ml-values to be freed in plan->to_be_freed
    // Set plan->execution_plan[n].free_from_index/free_to_index for every n that must free some ml-value.

    plan_.to_be_freed.reserve(freelist_.size());
    bool has_prev_dealloc_point = false;
    size_t prev_dealloc_point = 0;
    //TODO: should be size_t
    int current = 0;  // current index into the to_be_freed vector

    // Copy all items from freelist to to_be_freed in reverse order
    for (auto it = freelist_.rbegin(), end = freelist_.rend(); it != end; ++it) {
      plan_.to_be_freed.push_back(it->ml_value);
      //
      if (it->deallocate_point != prev_dealloc_point) {
        if (has_prev_dealloc_point)
          plan_.execution_plan[prev_dealloc_point].free_to_index = current - 1;
        prev_dealloc_point = it->deallocate_point;
        has_prev_dealloc_point = true;
        plan_.execution_plan[prev_dealloc_point].free_from_index = current;
      }
      current++;
    }

    if (has_prev_dealloc_point)
      plan_.execution_plan[prev_dealloc_point].free_to_index = current - 1;

    size_t program_counter = 0;
    for (auto& node_plan : plan_.execution_plan) {
      for (int index = node_plan.free_from_index; index <= node_plan.free_to_index; ++index) {
        auto ml_value_idx = plan_.to_be_freed[index];
        if (AllocPlan(ml_value_idx).alloc_kind == AllocKind::kAllocate) {
          ORT_ENFORCE(AllocPlan(ml_value_idx).program_counter.Ends().back() == program_counter);
        }
      }

      program_counter += 1;
    }
  }

  static bool IsNonTensor(const onnxruntime::NodeArg& nodearg) {
    // TODO: unclear why we should go through a string-representation of type
    auto ptype = nodearg.Type();
    auto& type_proto = ONNX_NAMESPACE::Utils::DataTypeUtils::ToTypeProto(ptype);
    return !utils::HasTensorType(type_proto);
  }

//For in-place reuse tensors, the lifetime is the union of all the tensors that tensors that use that buffer
#if !defined(ORT_MINIMAL_BUILD) && defined(ORT_MEMORY_PROFILE)
  void AdjustInplaceLifeIntervals() {
    std::unordered_map<OrtValueIndex, std::vector<OrtValueIndex>> inplace_reuse_buffer;
    for (size_t i = 0; i < ort_value_info_.size(); ++i) {
      if (AllocPlan(OrtValueIndex(i)).inplace_reuse != OrtValueIndex(i)) {
        inplace_reuse_buffer[ort_value_info_[i].inplace_reused_buffer_index].push_back(OrtValueIndex(i));
      }
    }
    for (const auto& item : inplace_reuse_buffer) {
      IntervalT& lifetime = AllocPlan(item.first).life_interval;
      for (const auto& value : item.second) {
        auto start = AllocPlan(value).life_interval.first;
        auto end = AllocPlan(value).life_interval.second;
        lifetime.first = lifetime.first < start ? lifetime.first : start;
        lifetime.second = lifetime.second > end ? lifetime.second : end;
      }
      for (const auto& value : item.second) {
        AllocPlan(value).life_interval = lifetime;
      }
    }
  }
#endif
};  // namespace onnxruntime

Status PlannerImpl::CreatePlan() {
  auto& p_graph_nodes = graph_viewer_.GetNodesInTopologicalOrder(context_.GetExecutionOrder());

  int num_ml_values = ort_value_name_idx_map_.MaxIdx() + 1;

  Initialize(p_graph_nodes.size(), static_cast<size_t>(num_ml_values));

  // Determine execution order: we use the default topological sort order for now. We can later
  // explore more efficient orderings (from a memory usage perspective).
  for (auto n : p_graph_nodes) {
    plan_.execution_plan.emplace_back(n);
  }

  // compute use counts for all ml-values
  ORT_RETURN_IF_ERROR(ComputeUseCounts());

  // determine sharing/reuse among ml-values
  ORT_RETURN_IF_ERROR(ComputeReusePlan());

  // Determine nodes that need fence check. This needs to be done after ComputeUseCounts and ComputeReusePlan.
  ORT_RETURN_IF_ERROR(ComputeFenceCheck());

#if !defined(ORT_MINIMAL_BUILD) && defined(ORT_MEMORY_PROFILE)
  //Adjust the allocate and lifetime intervals for all ml-values, based on their allocation kind.
  AdjustInplaceLifeIntervals();
#endif

#ifdef ENABLE_TRAINING
  // Determine allocation order for weights and activations. This needs to be done after ComputeReusePlan.
  ORT_RETURN_IF_ERROR(ComputeAllocationOrder());
#endif

  // convert information in the freelist_ into a deallocation plan in required format
  GenerateDeallocationPlan();

  // Ensure Memory-Time schedule is valid. This should be called at the end because memory start/end timestamps
  // are updated until GenerateDeallocationPlan is finished.
  VerifyMemoryTimeSchedule();

  return Status::OK();
}

Status SequentialPlanner::CreatePlan(
    const Node* parent_node,
    const onnxruntime::GraphViewer& graph_viewer,
    const std::vector<const NodeArg*>& outer_scope_node_args,
    const ExecutionProviders& providers,
    const KernelCreateInfoMap& kernel_create_info_map,
    const SubgraphsKernelCreateInfoMaps& subgraphs_kernel_create_info_maps,
    const std::unordered_map<std::string, std::reference_wrapper<const ExecutionProviders>>& subgraphs_execution_providers,
    const OrtValueNameIdxMap& ort_value_name_idx_map,
    const ISequentialPlannerContext& context,
    std::unique_ptr<SequentialExecutionPlan>& plan) {
  // allocate/reset here so we know it's clean
  plan = std::make_unique<SequentialExecutionPlan>();

  PlannerImpl planner(parent_node, graph_viewer, outer_scope_node_args, providers,
                      kernel_create_info_map, subgraphs_kernel_create_info_maps,
                      subgraphs_execution_providers,
                      ort_value_name_idx_map, context, *plan);

  return planner.CreatePlan();
}

}  // namespace onnxruntime
