#include "simcoe/render/graph.h"
#include "dx/d3d12.h"

using namespace simcoe;
using namespace simcoe::render;

namespace {
    std::string edgeName(InEdge *pEdge) {
        return std::format("in:{}:{}", pEdge->getPass()->getName(), pEdge->getName());
    }

    std::string edgeName(OutEdge *pEdge) {
        return std::format("out:{}:{}", pEdge->getPass()->getName(), pEdge->getName());
    }
}

GraphObject::GraphObject(const std::string& name, Graph& graph)
    : name(name)
    , graph(graph)
{ }

GraphObject::GraphObject(const GraphObject& other)
    : GraphObject(other.name, other.graph)
{ }

const char *GraphObject::getName() const {
    return name.c_str();
}

Graph& GraphObject::getGraph() const {
    return graph;
}

Context& GraphObject::getContext() const {
    return getGraph().getContext();
}

void InEdge::updateEdge(OutEdge *pEdge) {     
    ASSERTF(pEdge != nullptr, "null setResource on {}", edgeName(this));
    pSource = pEdge;
}

ID3D12Resource *InEdge::getResource() {
    return pSource->getResource();
}

D3D12_CPU_DESCRIPTOR_HANDLE InEdge::cpuHandle(D3D12_DESCRIPTOR_HEAP_TYPE type) {
    return pSource->cpuHandle(type);
}

D3D12_GPU_DESCRIPTOR_HANDLE InEdge::gpuHandle(D3D12_DESCRIPTOR_HEAP_TYPE type) {
    return pSource->gpuHandle(type);
}

ID3D12Resource *RelayEdge::getResource() {
    ID3D12Resource *pResource = pOther->getResource();
    ASSERTF(pResource != nullptr, "null resource returned from {} in relay {}", edgeName(pOther), edgeName(this));
    return pResource;
}

D3D12_RESOURCE_STATES RelayEdge::getState() const {
    return pOther->getState();
}

D3D12_CPU_DESCRIPTOR_HANDLE RelayEdge::cpuHandle(D3D12_DESCRIPTOR_HEAP_TYPE type) {
    return pOther->cpuHandle(type);
}

D3D12_GPU_DESCRIPTOR_HANDLE RelayEdge::gpuHandle(D3D12_DESCRIPTOR_HEAP_TYPE type) {
    return pOther->gpuHandle(type);
}

InEdge *Pass::addInput(InEdge *pEdge) {
    inputs.emplace_back(pEdge);
    return pEdge;
}

OutEdge *Pass::addOutput(OutEdge *pEdge) {
    outputs.emplace_back(pEdge);
    return pEdge;
}

Graph::Graph(Context& context)
    : context(context)
{ }

struct PassTree final {
    PassTree(Pass *pRoot)
        : pPass(pRoot) 
    { }

    void add(PassTree&& child) { children.push_back(std::move(child)); }

    Pass *pPass;
    std::vector<PassTree> children;
};

struct GraphBuilder final {
    GraphBuilder(Graph& graph, Pass *pRoot, ID3D12GraphicsCommandList* pCommands) : graph(graph) { 
        run(build(pRoot), pCommands);
    }

private:
    PassTree build(Pass *pRoot) {
        ASSERT(pRoot != nullptr);
        PassTree tree(pRoot);

        auto edges = graph.getEdges();

        for (auto& input : pRoot->getInputs()) {
            ASSERTF(edges.contains(input.get()), "edge {} was not found", edgeName(input.get()));

            auto& wire = edges.at(input.get());
            tree.add(build(wire->getPass()));
        }

        return tree;
    }

    void wireBarriers(Pass *pPass, ID3D12GraphicsCommandList *pCommands) {
        std::vector<D3D12_RESOURCE_BARRIER> barriers;
        
        // for each input edge
        for (auto& dest : pPass->getInputs()) {
            // find the source of the input
            auto& source = graph.getEdges().at(dest.get());
            
            dest->updateEdge(source);

            if (source->getState() == dest->getState()) { continue; }

            barriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(
                /* pResource = */ dest->getResource(),
                /* StateBefore = */ source->getState(),
                /* StateAfter = */ dest->getState()
            ));
        }

        if (!barriers.empty()) {
            pCommands->ResourceBarrier(
                /* NumBarriers = */ UINT(barriers.size()),
                /* pBarriers = */ barriers.data()
            );
        }
    }

    void run(const PassTree& tree, ID3D12GraphicsCommandList* pCommands) {
        auto& [pPass, deps] = tree;
        if (visited[pPass].test_and_set()) {
            return;
        }

        for (auto& dep : deps) {
            run(dep, pCommands);
        }

        wireBarriers(pPass, pCommands);

        pPass->execute(pCommands);
    }

    std::unordered_map<Pass*, std::atomic_flag> visited;
    Graph& graph;
};

void Graph::connect(OutEdge *pSource, InEdge *pTarget) {
    ASSERT(pSource != nullptr);
    ASSERT(pTarget != nullptr);

    edges[pTarget] = pSource;
}

void Graph::start() {
    commands = context.newCommandBuffer(D3D12_COMMAND_LIST_TYPE_DIRECT);
    
    for (auto& [pzName, pPass] : passes) {
        pPass->start(commands.pCommandList);
    }

    context.submitDirectCommands(commands);
}

void Graph::stop() {
    for (auto& [pzName, pPass] : passes) {
        pPass->stop();
    }
}

void Graph::execute(Pass *pRoot) {
    // TODO: track effects somehow
    ID3D12DescriptorHeap *ppHeaps[] = { context.getCbvHeap().getHeap() };
    commands.pCommandList->SetDescriptorHeaps(UINT(std::size(ppHeaps)), ppHeaps);

    GraphBuilder graph{*this, pRoot, commands.pCommandList};
    context.submitDirectCommands(commands);
    context.present();
}

Context& Graph::getContext() const {
    return context;
}
