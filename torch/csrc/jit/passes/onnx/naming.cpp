#include <torch/csrc/jit/passes/onnx/naming.h>
#include <torch/csrc/onnx/onnx.h>

namespace torch {
namespace jit {
namespace onnx {

namespace ONNXScopeName {

using NameFunc = std::string (*)(torch::jit::ScopePtr scope);

const std::string name_separator = "::";

namespace {

std::string nameFromRoot(
    torch::jit::ScopePtr scope,
    const std::string& layer_separator,
    NameFunc nameFunc) {
  std::string out = (*nameFunc)(scope);
  if (scope->isRoot()) {
    return out;
  }
  auto parent = scope->parent();
  while (!parent->isRoot()) {
    out = std::string((*nameFunc)(parent)).append(layer_separator).append(out);
    parent = parent->parent();
  }
  return out;
}

std::pair<std::string, std::string> parseNameFromScope(
    torch::jit::ScopePtr scope) {
  std::string full_name = scope->name().toUnqualString();
  auto pos = full_name.find(name_separator);
  TORCH_CHECK(
      pos != std::string::npos,
      "Scope name (" + full_name + ") does not contain '" + name_separator +
          "'");
  return std::make_pair(full_name.substr(0, pos), full_name.substr(pos + 2));
}

} // namespace

std::string createFullScopeName(
    const std::string& class_name,
    const std::string& variable_name) {
  return std::string(class_name).append(name_separator).append(variable_name);
}

std::string variableName(torch::jit::ScopePtr scope) {
  return parseNameFromScope(scope).second;
}

std::string variableNameFromRoot(
    torch::jit::ScopePtr scope,
    const std::string& layer_separator) {
  return nameFromRoot(scope, layer_separator, &variableName);
}

std::string className(torch::jit::ScopePtr scope) {
  return parseNameFromScope(scope).first;
}

std::string classNameFromRoot(
    torch::jit::ScopePtr scope,
    const std::string& layer_separator) {
  return nameFromRoot(scope, layer_separator, &className);
}

bool isCompatibleScope(torch::jit::ScopePtr scope) {
  return !scope->isRoot() && !scope->isBlank() &&
      (std::string(scope->name().toUnqualString()).find(name_separator) !=
       std::string::npos);
}
} // namespace ONNXScopeName

namespace {

class NodeNameGenerator {
 public:
  NodeNameGenerator(std::shared_ptr<Graph> g) : graph_(g){};
  virtual ~NodeNameGenerator() = 0;
  virtual void PopulateNodeNames() = 0;
  // TODO: don't need GetName probably.
  virtual c10::optional<std::string> GetName(const Node* n) const = 0;

 protected:
  virtual void PopulateNodeNames(Block*) = 0;
  virtual void CreateName(Node* n) = 0;
  void UpdateOutputsNames(Node* n);
  bool IsGraphOutput(const Value* v, const std::shared_ptr<Graph> graph) const;

 protected:
  std::string UpdateBaseName(
      std::unordered_map<std::string, size_t>& base_name_count,
      std::string base_name);

  std::unordered_map<const Node*, std::string> node_names_;
  std::unordered_map<std::string, size_t> base_node_name_counts_;
  std::shared_ptr<Graph> graph_;
  const std::string layer_separator_ = "/";
};
NodeNameGenerator::~NodeNameGenerator(){};

class ScopedNodeNameGenerator : public NodeNameGenerator {
 public:
  ScopedNodeNameGenerator(std::shared_ptr<Graph> g) : NodeNameGenerator(g){};
  void PopulateNodeNames() override;
  c10::optional<std::string> GetName(const Node* n) const override;

 protected:
  void PopulateNodeNames(Block*) override;
  void CreateName(Node* n) override;

 private:
  std::string GetFullScopeName(ScopePtr scope);
  std::unordered_map<ScopePtr, std::string> full_scope_names_;
  std::unordered_map<std::string, size_t> base_scope_name_counts_;
};

std::string NodeNameGenerator::UpdateBaseName(
    std::unordered_map<std::string, size_t>& base_name_count,
    std::string base_name) {
  if (base_name_count.find(base_name) == base_name_count.end()) {
    base_name_count[base_name] = 0;
  } else {
    auto count = ++base_name_count[base_name];
    base_name += "_";
    base_name += std::to_string(count);
  }
  return base_name;
}

bool NodeNameGenerator::IsGraphOutput(
    const Value* v,
    const std::shared_ptr<Graph> graph) const {
  for (const auto* graph_output : graph->outputs()) {
    if (v == graph_output) {
      return true;
    }
  }
  return false;
}

void NodeNameGenerator::UpdateOutputsNames(Node* n) {
  // auto node_name = this->GetName(n);
  if (node_names_.find(n) != node_names_.end()) {
    auto node_name = node_names_[n];
    for (auto i : c10::irange(n->outputs().size())) {
      auto output = n->output(i);
      if (!IsGraphOutput(output, graph_)) {
        auto output_name = node_name;
        output_name.append("_output_").append(std::to_string(i));
        output->setDebugName(output_name);
      }
    }
  }
}

void ScopedNodeNameGenerator::PopulateNodeNames() {
  PopulateNodeNames(graph_->block());
}

void ScopedNodeNameGenerator::PopulateNodeNames(Block* b) {
  for (auto* n : b->nodes()) {
    for (auto* sub_block : n->blocks()) {
      PopulateNodeNames(sub_block);
    }
    // TODO: better naming
    CreateName(n);
    UpdateOutputsNames(n);
  }
}

void ScopedNodeNameGenerator::CreateName(Node* n) {
  if (node_names_.find(n) == node_names_.end()) {
    if (!ONNXScopeName::isCompatibleScope(n->scope())) {
      return;
    }
    if (n->mustBeNone()) {
      // JIT IR does not allow attribute for None node.
      return;
    }
    auto name = GetFullScopeName(n->scope());
    name += layer_separator_;
    name += n->kind().toUnqualString();
    node_names_[n] = UpdateBaseName(base_node_name_counts_, name);
  }
  n->s_(Symbol::attr(::torch::onnx::OnnxNodeNameAttribute), node_names_[n]);
}

c10::optional<std::string> ScopedNodeNameGenerator::GetName(
    const Node* n) const {
  if (node_names_.find(n) == node_names_.end()) {
    return c10::nullopt;
  }
  return node_names_.at(n);
}

std::string ScopedNodeNameGenerator::GetFullScopeName(ScopePtr scope) {
  if (full_scope_names_.find(scope) == full_scope_names_.end()) {
    auto full_scope_name =
        ONNXScopeName::variableNameFromRoot(scope, layer_separator_);
    full_scope_names_[scope] =
        UpdateBaseName(base_scope_name_counts_, full_scope_name);
  }
  return full_scope_names_[scope];
}

} // namespace

void AssignNodeAndValueNames(std::shared_ptr<Graph>& graph) {
  auto node_name_generator = std::make_unique<ScopedNodeNameGenerator>(graph);
  node_name_generator->PopulateNodeNames();
}

} // namespace onnx
} // namespace jit
} // namespace torch
