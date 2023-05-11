// Copyright (C) 2023 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

#include "modelresourcemanagement.h"

#include <abstractview.h>
#include <bindingproperty.h>
#include <nodelistproperty.h>
#include <nodemetainfo.h>
#include <variantproperty.h>

#include <utils/algorithm.h>
#include <utils/set_algorithm.h>

#include <functional>

namespace QmlDesigner {

namespace {

enum class CheckRecursive { No, Yes };

class NodeActions;

template<typename ActionCall>
void forEachAction(NodeActions &nodeActions, ActionCall actionCall);

struct Base
{
    Base(ModelResourceSet &resourceSet, NodeActions &nodeActions)
        : resourceSet{resourceSet}
        , nodeActions{nodeActions}
    {}

    void removeNodes(QList<ModelNode> newModelNodes, CheckRecursive checkRecursive)
    {
        if (newModelNodes.empty())
            return;

        auto oldModelNodes = removeNodes(newModelNodes);

        if (checkRecursive == CheckRecursive::Yes)
            checkNewModelNodes(newModelNodes, oldModelNodes);
    }

    void checkModelNodes(QList<ModelNode> newModelNodes)
    {
        if (newModelNodes.empty())
            return;

        std::sort(newModelNodes.begin(), newModelNodes.end());

        checkNewModelNodes(newModelNodes, resourceSet.removeModelNodes);
    }

    void removeProperties(AbstractProperties newProperties, CheckRecursive checkRecursive)
    {
        if (newProperties.empty())
            return;

        auto oldProperties = removeProperties(newProperties);

        if (checkRecursive == CheckRecursive::Yes)
            checkNewProperties(newProperties, oldProperties);
    }

    void addSetExpressions(ModelResourceSet::SetExpressions newSetExpressions)
    {
        auto &setExpressions = resourceSet.setExpressions;
        setExpressions.append(std::move(newSetExpressions));
    }

    void handleNodes(const ModelNodes &) {}

    void handleProperties(const AbstractProperties &) {}

    void finally() {}

private:
    ModelNodes removeNodes(ModelNodes &newModelNodes)
    {
        std::sort(newModelNodes.begin(), newModelNodes.end());

        auto oldModelNodes = std::move(resourceSet.removeModelNodes);
        resourceSet.removeModelNodes = {};
        resourceSet.removeModelNodes.reserve(oldModelNodes.size() + newModelNodes.size());

        std::set_union(newModelNodes.begin(),
                       newModelNodes.end(),
                       oldModelNodes.begin(),
                       oldModelNodes.end(),
                       std::back_inserter(resourceSet.removeModelNodes));

        return oldModelNodes;
    }

    AbstractProperties removeProperties(AbstractProperties &newProperties)
    {
        std::sort(newProperties.begin(), newProperties.end());

        auto oldProperties = std::move(resourceSet.removeProperties);
        resourceSet.removeProperties = {};
        resourceSet.removeProperties.reserve(oldProperties.size() + newProperties.size());

        std::set_union(newProperties.begin(),
                       newProperties.end(),
                       oldProperties.begin(),
                       oldProperties.end(),
                       std::back_inserter(resourceSet.removeProperties));

        return oldProperties;
    }

    void checkNewModelNodes(const QList<ModelNode> &newModelNodes,
                            const QList<ModelNode> &oldModelNodes)
    {
        ModelNodes addedModelNodes;
        addedModelNodes.reserve(newModelNodes.size());

        std::set_difference(newModelNodes.begin(),
                            newModelNodes.end(),
                            oldModelNodes.begin(),
                            oldModelNodes.end(),
                            std::back_inserter(addedModelNodes));

        if (addedModelNodes.size())
            forEachAction(nodeActions, [&](auto &action) { action.handleNodes(addedModelNodes); });
    }

    void checkNewProperties(const AbstractProperties &newProperties,
                            const AbstractProperties &oldProperties)
    {
        AbstractProperties addedProperties;
        addedProperties.reserve(newProperties.size());

        std::set_difference(newProperties.begin(),
                            newProperties.end(),
                            oldProperties.begin(),
                            oldProperties.end(),
                            std::back_inserter(addedProperties));

        if (addedProperties.size())
            forEachAction(nodeActions,
                          [&](auto &action) { action.handleProperties(addedProperties); });
    }

private:
    ModelResourceSet &resourceSet;
    NodeActions &nodeActions;
};

struct CheckChildNodes : public Base
{
    using Base::Base;

    void handleNodes(const ModelNodes &nodes)
    {
        ModelNodes childNodes;
        for (const ModelNode &node : nodes)
            childNodes.append(node.directSubModelNodes());

        checkModelNodes(childNodes);
    }
};

struct CheckNodesInNodeAbstractProperties : public Base
{
    using Base::Base;

    ModelNodes collectNodes(const AbstractProperties &properties)
    {
        ModelNodes modelNodes;

        for (const AbstractProperty &property : properties) {
            if (property.isNodeAbstractProperty())
                modelNodes.append(property.toNodeAbstractProperty().directSubNodes());
        }

        return modelNodes;
    }

    void handleProperties(const AbstractProperties &properties)
    {
        checkModelNodes(collectNodes(properties));
    }
};

struct RemoveLayerEnabled : public Base
{
    using Base::Base;

    AbstractProperties collectProperties(const ModelNodes &nodes)
    {
        AbstractProperties properties;

        for (const ModelNode &node : nodes) {
            if (node.parentProperty().name() == "layer.effect") {
                auto layerEnabledProperty = node.parentProperty().parentModelNode().property(
                    "layer.enabled");
                if (layerEnabledProperty.exists())
                    properties.push_back(layerEnabledProperty);
            }
        }

        return properties;
    }

    void handleNodes(const ModelNodes &nodes)
    {
        removeProperties(collectProperties(nodes), CheckRecursive::No);
    }
};

struct RemoveAliasExports : public Base
{
    RemoveAliasExports(ModelResourceSet &resourceSet, NodeActions &nodeActions, ModelNode rootNode)
        : Base{resourceSet, nodeActions}
        , rootNode{std::move(rootNode)}
    {}

    AbstractProperties collectProperties(const ModelNodes &nodes)
    {
        AbstractProperties properties;

        for (const ModelNode &node : nodes) {
            PropertyName propertyName = node.id().toUtf8();

            if (rootNode.bindingProperty(propertyName).isAliasExport())
                properties.push_back(rootNode.property(propertyName));
        }

        return properties;
    }

    void handleNodes(const ModelNodes &nodes)
    {
        removeProperties(collectProperties(nodes), CheckRecursive::Yes);
    }

    ModelNode rootNode;
};

struct NodeDependency
{
    ModelNode target;
    ModelNode source;

    friend bool operator<(const NodeDependency &first, const NodeDependency &second)
    {
        return std::tie(first.target, first.source) < std::tie(second.target, second.source);
    }

    friend bool operator<(const NodeDependency &first, const ModelNode &second)
    {
        return first.target < second;
    }

    friend bool operator<(const ModelNode &first, const NodeDependency &second)
    {
        return first < second.target;
    }
};

using NodeDependencies = std::vector<NodeDependency>;

struct NameNode
{
    QString name;
    ModelNode node;

    friend bool operator<(const NameNode &first, const NameNode &second)
    {
        return first.name < second.name;
    }
};

using NameNodes = std::vector<NameNode>;

struct NodesProperty
{
    ModelNode source;
    PropertyName name;
    ModelNodes targets;
    bool isChanged = false;

    friend bool operator<(const NodesProperty &first, const NodesProperty &second)
    {
        return first.source < second.source;
    }
};

using NodesProperties = std::vector<NodesProperty>;

struct RemoveDependencies : public Base
{
    RemoveDependencies(ModelResourceSet &resourceSet,
                       NodeActions &nodeActions,
                       NodeDependencies dependencies)
        : Base{resourceSet, nodeActions}
        , dependencies{std::move(dependencies)}
    {}

    ModelNodes collectNodes(const ModelNodes &nodes) const
    {
        ModelNodes targetNodes;
        ::Utils::set_greedy_intersection(dependencies.begin(),
                                         dependencies.end(),
                                         nodes.begin(),
                                         nodes.end(),
                                         ::Utils::make_iterator([&](const NodeDependency &dependency) {
                                             targetNodes.push_back(dependency.source);
                                         }));

        return targetNodes;
    }

    void handleNodes(const ModelNodes &nodes)
    {
        removeNodes(collectNodes(nodes), CheckRecursive::No);
    }

    NodeDependencies dependencies;
};

struct RemoveTargetsSources : public Base
{
    RemoveTargetsSources(ModelResourceSet &resourceSet,
                         NodeActions &nodeActions,
                         NodeDependencies dependencies,
                         NodesProperties nodesProperties)
        : Base{resourceSet, nodeActions}
        , dependencies{std::move(dependencies)}
        , nodesProperties{std::move(nodesProperties)}
    {}

    static void removeDependency(NodesProperties &removedTargetNodesInProperties,
                                 const NodeDependency &dependency)
    {
        auto found = std::find_if(removedTargetNodesInProperties.begin(),
                                  removedTargetNodesInProperties.end(),
                                  [&](const auto &nodeProperty) {
                                      return nodeProperty.source == dependency.source;
                                  });

        if (found == removedTargetNodesInProperties.end())
            removedTargetNodesInProperties.push_back({dependency.source, "", {dependency.target}});
        else
            found->targets.push_back(dependency.target);
    }

    NodesProperties collectRemovedDependencies(const ModelNodes &nodes)
    {
        NodesProperties removedTargetNodesInProperties;

        ModelNodes targetNodes;
        ::Utils::set_greedy_intersection(dependencies.begin(),
                                         dependencies.end(),
                                         nodes.begin(),
                                         nodes.end(),
                                         ::Utils::make_iterator([&](const NodeDependency &dependency) {
                                             removeDependency(removedTargetNodesInProperties,
                                                              dependency);
                                         }));

        std::sort(removedTargetNodesInProperties.begin(), removedTargetNodesInProperties.end());

        return removedTargetNodesInProperties;
    }

    ModelNodes collectNodesToBeRemoved(const ModelNodes &nodes)
    {
        ModelNodes nodesToBeRemoved;

        auto removeTargets = [&](auto &first, auto &second) {
            auto newEnd = std::remove_if(first.targets.begin(),
                                         first.targets.end(),
                                         [&](const ModelNode &node) {
                                             return std::find(second.targets.begin(),
                                                              second.targets.end(),
                                                              node)
                                                    != second.targets.end();
                                         });
            if (newEnd != first.targets.end()) {
                first.isChanged = true;
                first.targets.erase(newEnd, first.targets.end());

                if (first.targets.empty())
                    nodesToBeRemoved.push_back(first.source);
            }
        };

        NodesProperties removedTargetNodesInProperties = collectRemovedDependencies(nodes);
        ::Utils::set_intersection_compare(nodesProperties.begin(),
                                          nodesProperties.end(),
                                          removedTargetNodesInProperties.begin(),
                                          removedTargetNodesInProperties.end(),
                                          removeTargets,
                                          std::less<NodesProperty>{});

        return nodesToBeRemoved;
    }

    void handleNodes(const ModelNodes &nodes)
    {
        removeNodes(collectNodesToBeRemoved(nodes), CheckRecursive::No);
    }

    QString createExpression(const NodesProperty &nodesProperty)
    {
        QString expression = "[";
        const ModelNode &last = nodesProperty.targets.back();
        for (const ModelNode &node : nodesProperty.targets) {
            expression += node.id();
            if (node != last)
                expression += ", ";
        }
        expression += "]";

        return expression;
    }

    void finally()
    {
        ModelResourceSet::SetExpressions setExpressions;

        for (const NodesProperty &nodesProperty : nodesProperties) {
            if (nodesProperty.isChanged && nodesProperty.targets.size()) {
                setExpressions.push_back({nodesProperty.source.bindingProperty(nodesProperty.name),
                                          createExpression(nodesProperty)});
            }
        }

        addSetExpressions(std::move(setExpressions));
    }

    NodeDependencies dependencies;
    NodesProperties nodesProperties;
};

struct DependenciesSet
{
    NodeDependencies nodeDependencies;
    NodeDependencies targetsDependencies;
    NodesProperties targetsNodesProperties;
};

template<typename Predicate>
struct TargetFilter
{
    TargetFilter(Predicate predicate, NodeDependencies &dependencies)
        : predicate{std::move(predicate)}
        , dependencies{dependencies}
    {}

    static std::optional<ModelNode> resolveTarget(const ModelNode &node)
    {
        auto targetProperty = node.bindingProperty("target");
        if (targetProperty.exists()) {
            if (ModelNode targetNode = targetProperty.resolveToModelNode())
                return targetNode;
        }

        return {};
    }

    void operator()(const NodeMetaInfo &metaInfo, const ModelNode &node)
    {
        if (predicate(metaInfo)) {
            if (auto targetNode = resolveTarget(node))
                dependencies.push_back({std::move(*targetNode), node});
        }
    }

    void finally() { std::sort(dependencies.begin(), dependencies.end()); }

    Predicate predicate;
    NodeDependencies &dependencies;
};

template<typename Predicate>
struct TargetsFilter
{
    TargetsFilter(Predicate predicate,
                  NodeDependencies &dependencies,
                  NodesProperties &targetsNodesProperties)
        : predicate{std::move(predicate)}
        , dependencies{dependencies}
        , targetsNodesProperties{targetsNodesProperties}
    {}

    static ModelNodes resolveTargets(const ModelNode &node)
    {
        auto targetProperty = node.bindingProperty("targets");
        if (targetProperty.exists())
            return targetProperty.resolveToModelNodeList();

        return {};
    }

    void operator()(const NodeMetaInfo &metaInfo, const ModelNode &node)
    {
        if (predicate(metaInfo)) {
            const auto targetNodes = resolveTargets(node);
            if (targetNodes.size()) {
                targetsNodesProperties.push_back({node, "targets", targetNodes});
                for (auto &&targetNode : targetNodes)
                    dependencies.push_back({targetNode, node});
            }
        }
    }

    void finally()
    {
        std::sort(dependencies.begin(), dependencies.end());
        std::sort(targetsNodesProperties.begin(), targetsNodesProperties.end());
    }

    Predicate predicate;
    NodeDependencies &dependencies;
    NodesProperties &targetsNodesProperties;
};

void addDependency(NameNodes &dependencies, const ModelNode &node, const PropertyName &propertyName)
{
    if (auto property = node.variantProperty(propertyName); property.exists()) {
        QString stateName = property.value().toString();
        if (stateName.size() && stateName != "*")
            dependencies.push_back({stateName, node});
    }
}

struct StateFilter
{
    StateFilter(NameNodes &dependencies)
        : dependencies{dependencies}
    {}

    void operator()(const NodeMetaInfo &metaInfo, const ModelNode &node)
    {
        if (metaInfo.isQtQuickState())
            addDependency(dependencies, node, "name");
    }

    void finally() { std::sort(dependencies.begin(), dependencies.end()); }

    NameNodes &dependencies;
};

struct TransitionFilter
{
    TransitionFilter(NodeDependencies &dependencies, NameNodes &stateNodes)
        : stateNodes{stateNodes}
        , dependencies{dependencies}
    {}

    void operator()(const NodeMetaInfo &metaInfo, const ModelNode &node)
    {
        if (metaInfo.isQtQuickTransition()) {
            addDependency(transitionNodes, node, "to");
            addDependency(transitionNodes, node, "from");
        }
    }

    void finally()
    {
        std::sort(transitionNodes.begin(), transitionNodes.end());

        auto removeTransition = [&](const auto &first, const auto &second) {
            dependencies.push_back({second.node, first.node});
        };

        ::Utils::set_greedy_intersection_compare(transitionNodes.begin(),
                                                 transitionNodes.end(),
                                                 stateNodes.begin(),
                                                 stateNodes.end(),
                                                 removeTransition,
                                                 std::less<NameNode>{});
        std::sort(dependencies.begin(), dependencies.end());
    }

    NameNodes transitionNodes;
    NameNodes &stateNodes;
    NodeDependencies &dependencies;
};

DependenciesSet createDependenciesSet(Model *model)
{
    const ModelNodes nodes = model->allModelNodesUnordered();

    DependenciesSet set;
    NameNodes stateNames;

    auto flowViewFlowActionAreaMetaInfo = model->flowViewFlowActionAreaMetaInfo();
    auto flowViewFlowDecisionMetaInfo = model->flowViewFlowDecisionMetaInfo();
    auto flowViewFlowWildcardMetaInfo = model->flowViewFlowWildcardMetaInfo();
    auto qtQuickPropertyChangesMetaInfo = model->qtQuickPropertyChangesMetaInfo();
    auto qtQuickTimelineKeyframeGroupMetaInfo = model->qtQuickTimelineKeyframeGroupMetaInfo();
    auto qtQuickPropertyAnimationMetaInfo = model->qtQuickPropertyAnimationMetaInfo();

    auto filters = std::make_tuple(
        TargetFilter{[&](auto &&metaInfo) {
                         return metaInfo.isBasedOn(qtQuickPropertyChangesMetaInfo,
                                                   qtQuickTimelineKeyframeGroupMetaInfo,
                                                   flowViewFlowActionAreaMetaInfo,
                                                   qtQuickPropertyAnimationMetaInfo);
                     },
                     set.nodeDependencies},
        TargetsFilter{[&](auto &&metaInfo) {
                          return metaInfo.isBasedOn(flowViewFlowDecisionMetaInfo,
                                                    flowViewFlowWildcardMetaInfo,
                                                    qtQuickPropertyAnimationMetaInfo);
                      },
                      set.targetsDependencies,
                      set.targetsNodesProperties},
        StateFilter{stateNames},
        TransitionFilter{set.nodeDependencies, stateNames});

    for (const ModelNode &node : nodes) {
        auto metaInfo = node.metaInfo();
        std::apply([&](auto &&...filter) { (filter(metaInfo, node), ...); }, filters);
    }

    std::apply([&](auto &&...filter) { (filter.finally(), ...); }, filters);

    return set;
}

using NodeActionsTuple = std::tuple<CheckChildNodes,
                                    CheckNodesInNodeAbstractProperties,
                                    RemoveLayerEnabled,
                                    RemoveAliasExports,
                                    RemoveDependencies,
                                    RemoveTargetsSources>;

class NodeActions : public NodeActionsTuple
{
    NodeActions(const NodeActions &) = delete;
    NodeActions &opertor(const NodeActions &) = delete;
    NodeActions(NodeActions &&) = delete;
    NodeActions &opertor(NodeActions &&) = delete;

    using NodeActionsTuple::NodeActionsTuple;
};

template<typename ActionCall>
void forEachAction(NodeActions &nodeActions, ActionCall actionCall)
{
    std::apply([&](auto &...action) { (actionCall(action), ...); },
               static_cast<NodeActionsTuple &>(nodeActions));
}

} // namespace

ModelResourceSet ModelResourceManagement::removeNode(const ModelNode &node) const
{
    ModelResourceSet resourceSet;
    Model *model = node.model();

    DependenciesSet set = createDependenciesSet(model);

    NodeActions nodeActions = {CheckChildNodes{resourceSet, nodeActions},
                               CheckNodesInNodeAbstractProperties{resourceSet, nodeActions},
                               RemoveLayerEnabled{resourceSet, nodeActions},
                               RemoveAliasExports{resourceSet, nodeActions, model->rootModelNode()},
                               RemoveDependencies{resourceSet,
                                                  nodeActions,
                                                  std::move(set.nodeDependencies)},
                               RemoveTargetsSources{resourceSet,
                                                    nodeActions,
                                                    std::move(set.targetsDependencies),
                                                    std::move(set.targetsNodesProperties)}};

    Base{resourceSet, nodeActions}.removeNodes({node}, CheckRecursive::Yes);

    forEachAction(nodeActions, [&](auto &action) { action.finally(); });

    return resourceSet;
}

ModelResourceSet ModelResourceManagement::removeProperty(const AbstractProperty &property) const
{
    ModelResourceSet resourceSet;
    Model *model = property.model();

    DependenciesSet set = createDependenciesSet(model);

    NodeActions nodeActions = {CheckChildNodes{resourceSet, nodeActions},
                               CheckNodesInNodeAbstractProperties{resourceSet, nodeActions},
                               RemoveLayerEnabled{resourceSet, nodeActions},
                               RemoveAliasExports{resourceSet, nodeActions, model->rootModelNode()},
                               RemoveDependencies{resourceSet,
                                                  nodeActions,
                                                  std::move(set.nodeDependencies)},
                               RemoveTargetsSources{resourceSet,
                                                    nodeActions,
                                                    std::move(set.targetsDependencies),
                                                    std::move(set.targetsNodesProperties)}};

    Base{resourceSet, nodeActions}.removeProperties({property}, CheckRecursive::Yes);

    forEachAction(nodeActions, [&](auto &action) { action.finally(); });

    return resourceSet;
}

} // namespace QmlDesigner
