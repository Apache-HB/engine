#pragma once

#include "engine/base/logging.h"
#include "engine/render/render.h"

#include <unordered_map>

namespace simcoe::render {
    struct Resource;
    struct Pass;
    struct Graph;
    struct Input;
    struct Output;

    using State = rhi::Buffer::State;
    using Barriers = std::vector<rhi::StateTransition>;

    struct GraphObject {
        GraphObject(Graph *parent, const char *name);

        Graph& getParent() const;
        const char *getName() const;
        Context& getContext() const;

    private:
        Graph *parent;
        const char *name;
    };

#if 0
    struct Handle {
        Handle(const Info& info)
            : info(info)
        { }

        virtual ~Handle() = default;

        virtual rhi::Buffer& getBuffer() = 0;
        virtual rhi::CpuHandle gpuHandle() = 0;
        virtual rhi::GpuHandle cpuHandle() = 0;

        Graph *getParent() const { return info.parent; }
        const char *getName() const { return info.name; }
        Context& getContext() const { return getContext(info); }

    private:
        Info info;
    };
#endif

    struct Resource : GraphObject {
        Resource(const GraphObject& info)
            : GraphObject(info)
        { }

        virtual ~Resource() = default;

        virtual void addBarrier(Barriers& barriers, Output* before, Input* after);
    };

    struct Wire {
        virtual ~Wire() = default;

        const char *getName() const { return name; }
        Pass *getPass() const { return pass; }
        State getState() const { return state; }

        Resource *resource;

    protected:
        Wire(Pass *pass, const char *name, State state, Resource *resource)
            : resource(resource)
            , name(name)
            , pass(pass)
            , state(state)
        { }

    private:
        const char *name;
        Pass *pass;
        rhi::Buffer::State state;
    };

    struct Input : Wire {
        using Wire::Wire;
        Input(Pass *pass, const char *name, State state = State::eInvalid)
            : Wire(pass, name, state, nullptr)
        { }
    };

    struct Output : Wire {
        using Wire::Wire;
        Output(Pass *pass, const char *name, State state = State::eInvalid)
            : Wire(pass, name, state, nullptr)
        { }
        
        virtual void update(Barriers& barriers, Input* input);
    };

    struct Source : Output {
        Source(Pass *pass, const char *name, State initial, Resource *resource) : Output(pass, name, initial, resource) { 
            ASSERTF(resource != nullptr, "source `{}` was null", name);
        }
    };

    struct Relay : Output {
        Relay(Pass *pass, const char *name, Input *input) 
            : Output(pass, name, input->getState(), input->resource)
            , input(input)
        { }

        void update(Barriers& barriers, Input* other) override;

    private:
        Input *input;
    };

    template<typename TWire, typename TResource>
    struct WireHandle {
        WireHandle(TWire *wire = nullptr) : wire(wire) { }

        TResource *get() { return static_cast<TResource*>(wire->resource); }
        TResource *operator->() { return get(); }

        operator TWire *() { return wire; }

    private:
        TWire *wire;
    };

    struct Pass : GraphObject {
        Pass(const GraphObject& info)
            : Pass(info, CommandSlot::eTotal)
        { }

        Pass(const GraphObject& info, CommandSlot::Slot slot)
            : GraphObject(info)
            , slot(slot)
        { }

        virtual ~Pass() = default;

        virtual void init() { }
        virtual void deinit() { }

        virtual void imgui() { }

        virtual void execute() = 0;

        template<typename T, typename... A>  requires (std::is_base_of_v<Input, T>)
        T *newInput(const char *name, A&&... args) {
            auto input = new T(this, name, std::forward<A>(args)...);
            inputs.push_back(input);
            return input;
        }

        template<typename T, typename... A> requires (std::is_base_of_v<Output, T>)
        T *newOutput(const char *name, A&&... args) {
            auto output = new T(this, name, std::forward<A>(args)...);
            outputs.push_back(output);
            return output;
        }

        std::vector<UniquePtr<Input>> inputs;
        std::vector<UniquePtr<Output>> outputs;

        CommandSlot::Slot getSlot() const { return slot; }

    protected:
        rhi::CommandList& getCommands();

    private:
        CommandSlot::Slot slot;
    };

    // render graph
    struct Graph {
        Graph(Context& ctx)
            : ctx(ctx)
        { }

        void init();
        void deinit();

        void execute(Pass *root);

        template<typename T, typename... A> requires (std::is_base_of_v<Pass, T>)
        T *addPass(const char *name, A&&... args) {
            auto pass = new T({ this, name }, std::forward<A>(args)...);
            passes[name] = pass;
            return pass;
        }

        template<typename T, typename... A> requires (std::is_base_of_v<Resource, T>)
        T *addResource(const char *name, A&&... args) {
            auto resource = new T({ this, name }, std::forward<A>(args)...);
            resources[name] = resource;
            return resource;
        }

        void link(Input *input, Output *output) {
            wires[input] = output;
        }

        template<typename T, typename... A> requires (std::is_base_of_v<Output, T>)
        UniquePtr<T> provide(const char *name, Resource *resource) {
            return new T(nullptr, name, resource);
        }

        // TODO: this probably shouldnt be public
        std::unordered_map<const char*, UniquePtr<Pass>> passes;
        std::unordered_map<const char*, UniquePtr<Resource>> resources;
        std::unordered_map<Input*, Output*> wires; // map of input to the output it is reading from
    
        logging::IChannel& channel = logging::v2::get(logging::eRender);

        Context& getContext() const { return ctx; }

    protected:
        Context& ctx;
    };
}
