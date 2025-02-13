#include "engine/render/scene.h"
#include "engine/render/render.h"

#include "engine/render/resources/resources.h"

#include "engine/render/passes/present.h"
#include "engine/render/passes/global.h"

#include "engine/assets/assets.h"
#include "imgui/imgui.h"
#include "imnodes/imnodes.h"

#include <array>

using namespace simcoe;
using namespace simcoe::render;

namespace {
    constexpr math::float4 kClearColour = { 0.f, 0.2f, 0.4f, 1.f };
    constexpr math::float4 kLetterBox = { 0.f, 0.f, 0.f, 1.f };
}

struct AsyncDraw {
    using Action = std::function<void()>;

    void add(Action action) {
        std::lock_guard lock(mutex);
        commands.push_back(std::move(action));
    }

    void apply() {
        std::lock_guard lock(mutex);
        for (auto& command : commands) {
            command();
        }
    }

private:
    std::mutex mutex;
    std::vector<Action> commands;
};

struct ScenePass final : Pass, assets::IWorldSink {
    ScenePass(const GraphObject& info) 
        : Pass(info, CommandSlot::eScene) 
        , textures(512)
    {
        sceneTargetResource = getParent().addResource<SceneTargetResource>("scene-target", State::eRenderTarget);

        sceneTargetOut = newOutput<Source>("scene-target", State::eRenderTarget, sceneTargetResource);
    }

    void init() override { }

    void execute() override {
        auto& ctx = getContext();
        auto& cmd = getCommands();

        auto [width, height] = ctx.sceneSize().as<float>();

        cmd.setViewAndScissor(rhi::View(0, 0, width, height));
        cmd.clearRenderTarget(sceneTargetResource->rtvHandle(), kClearColour);
    
        draws.apply();
    }

    void imgui() override {
        if (ImGui::Begin("Scene")) {
            static char path[1024] {};
            ImGui::InputTextWithHint("##", "gltf path", path, sizeof(path));
            
            if (ImGui::Button("Load model")) {
                loadGltfAsync(path);
            }
        }
        ImGui::End();
    }

    SceneTargetResource *sceneTargetResource;
    Output *sceneTargetOut;

private:
    AsyncDraw draws;

    constexpr static auto kSamplers = std::to_array<rhi::Sampler>({
        { rhi::ShaderVisibility::ePixel }
    });

    constexpr static auto kDebugBindings = std::to_array<rhi::BindingRange>({
        { .base = 0, .type = rhi::Object::eConstBuffer, .mutability = rhi::BindingMutability::eAlwaysStatic, .space = 1 }
    });

    constexpr static auto kSceneBufferBindings = std::to_array<rhi::BindingRange>({
        { 0, rhi::Object::eConstBuffer, rhi::BindingMutability::eStaticAtExecute }
    });

    constexpr static auto kObjectBufferBindings = std::to_array<rhi::BindingRange>({
        { 1, rhi::Object::eConstBuffer, rhi::BindingMutability::eStaticAtExecute }
    });

    constexpr static auto kSceneTextureBindings = std::to_array<rhi::BindingRange>({
        { 0, rhi::Object::eTexture, rhi::BindingMutability::eBindless, SIZE_MAX }
    });

    constexpr static auto kSceneBindings = std::to_array<rhi::Binding>({
        rhi::bindTable(rhi::ShaderVisibility::ePixel, kDebugBindings), // register(b0, space1) is debug data

        rhi::bindTable(rhi::ShaderVisibility::eAll, kSceneBufferBindings), // register(b0) is per scene data
        rhi::bindTable(rhi::ShaderVisibility::eVertex, kObjectBufferBindings), // register(b1) is per object data
        rhi::bindConst(rhi::ShaderVisibility::ePixel, 2, 1), // register(b2) is per primitive data
        rhi::bindTable(rhi::ShaderVisibility::ePixel, kSceneTextureBindings) // register(t0...) are all the textures
    });

    constexpr static auto kInputLayout = std::to_array<rhi::InputElement>({
        { "POSITION", rhi::Format::float32x3, offsetof(assets::Vertex, position) },
        { "NORMAL", rhi::Format::float32x3, offsetof(assets::Vertex, normal) },
        { "TEXCOORD", rhi::Format::float32x2, offsetof(assets::Vertex, uv) }
    });

public:
    size_t addVertexBuffer(assets::VertexBuffer&&) override {
        return SIZE_MAX;
    }

    size_t addIndexBuffer(assets::IndexBuffer&&) override {
        return SIZE_MAX;
    }

    size_t addTexture(const assets::Texture& texture) override {
        auto& ctx = getContext();
        auto& heap = ctx.getHeap();

        size_t slot = heap.alloc(DescriptorSlot::eTexture);
        if (slot == SIZE_MAX) { return SIZE_MAX; }

        size_t index = textures.alloc(slot);
        if (index == SIZE_MAX) { return SIZE_MAX; }

        auto& copy = ctx.getCopyQueue();

        // TODO: this doesnt feel right
        auto& [id, size, data] = texture;
        auto upload = copy.beginTextureUpload(data.data(), data.size(), size);

        copy.submit(upload);

        return index;
    }

    size_t addPrimitive(const assets::Primitive&) override {
        return SIZE_MAX;
    }

    size_t addNode(const assets::Node&) override {
        return SIZE_MAX;
    }

private:
    using IndexMap = memory::AtomicSlotMap<size_t, SIZE_MAX>;
    
    IndexMap textures;
};

struct PostPass final : Pass {
    PostPass(const GraphObject& info) : Pass(info, CommandSlot::ePost) {
        sceneTargetIn = newInput<Input>("scene-target", State::ePixelShaderResource);
        sceneTargetOut = newOutput<Relay>("scene-target", sceneTargetIn); 
        
        renderTargetIn = newInput<Input>("rtv", State::eRenderTarget);
        renderTargetOut = newOutput<Relay>("rtv", renderTargetIn);
    }

    void init() override {
        initView();
        initScreenQuad();
    }

    void execute() override {
        auto& cmd = getCommands();
        auto *renderTarget = renderTargetIn.get();

        cmd.setViewAndScissor(view);
        cmd.setRenderTarget(renderTarget->rtvCpuHandle(), rhi::CpuHandle::Invalid, kLetterBox);
        
        draws.apply();
    }

    WireHandle<Input, SceneTargetResource> sceneTargetIn;
    WireHandle<Input, RenderTargetResource> renderTargetIn;

    Output *renderTargetOut;
    Output *sceneTargetOut;

private:
    void initView() {
        auto& ctx = getContext();
        auto [sceneWidth, sceneHeight] = ctx.sceneSize();
        auto [windowWidth, windowHeight] = ctx.windowSize();

        auto widthRatio = float(sceneWidth) / windowWidth;
        auto heightRatio = float(sceneHeight) / windowHeight;

        float x = 1.f;
        float y = 1.f;

        if (widthRatio < heightRatio) {
            x = widthRatio / heightRatio;
        } else {
            y = heightRatio / widthRatio;
        }

        view.viewport.TopLeftX = windowWidth * (1.f - x) / 2.f;
        view.viewport.TopLeftY = windowHeight * (1.f - y) / 2.f;
        view.viewport.Width = x * windowWidth;
        view.viewport.Height = y * windowHeight;

        view.scissor.left = LONG(view.viewport.TopLeftX);
        view.scissor.right = LONG(view.viewport.TopLeftX + view.viewport.Width);
        view.scissor.top = LONG(view.viewport.TopLeftY);
        view.scissor.bottom = LONG(view.viewport.TopLeftY + view.viewport.Height);
    }

    void initScreenQuad() {
        auto& ctx = getContext();
        auto& device = ctx.getDevice();
        auto& copy = ctx.getCopyQueue();

        auto ps = loadShader("resources/shaders/post-shader.ps.pso");
        auto vs = loadShader("resources/shaders/post-shader.vs.pso");

        pso = device.newPipelineState({
            .samplers = kSamplers,
            .bindings = kBindings,
            .input = kInput,
            .ps = ps,
            .vs = vs
        });

        auto vboCopy = copy.beginDataUpload(kScreenQuad, sizeof(kScreenQuad));
        auto iboCopy = copy.beginDataUpload(kScreenQuadIndices, sizeof(kScreenQuadIndices));

        auto batchCopy = copy.beginBatchUpload({ vboCopy, iboCopy });

        copy.submit(batchCopy, [&, vboCopy, iboCopy] {
            vbo = vboCopy->getBuffer();
            ibo = iboCopy->getBuffer();

            rhi::VertexBufferView vboView = rhi::newVertexBufferView(vbo.gpuAddress(), std::size(kScreenQuad), sizeof(assets::Vertex));
            rhi::IndexBufferView iboView = {
                .buffer = ibo.gpuAddress(),
                .length = std::size(kScreenQuadIndices),
                .format = rhi::Format::uint32
            };

            draws.add([&, vboView, iboView] {
                auto& cmd = getCommands();
                auto* sceneTarget = sceneTargetIn.get();

                cmd.setPipeline(pso);
                cmd.bindTable(0, sceneTarget->cbvGpuHandle());
                cmd.setVertexBuffers(std::to_array({ vboView }));
                cmd.drawIndexed(iboView);
            });
        });
    }

    rhi::View view;

    rhi::PipelineState pso;
    rhi::Buffer vbo;
    rhi::Buffer ibo;

    AsyncDraw draws;

    constexpr static assets::Vertex kScreenQuad[] = {
        { .position = { -1.0f, -1.0f, 0.0f }, .uv = { 0.0f, 1.0f } }, // bottom left
        { .position = { -1.0f, 1.0f,  0.0f }, .uv = { 0.0f, 0.0f } },  // top left
        { .position = { 1.0f,  -1.0f, 0.0f }, .uv = { 1.0f, 1.0f } },  // bottom right
        { .position = { 1.0f,  1.0f,  0.0f }, .uv = { 1.0f, 0.0f } } // top right
    };

    constexpr static uint32_t kScreenQuadIndices[] = {
        0, 1, 2,
        1, 3, 2
    };
    
    constexpr static auto kInput = std::to_array<rhi::InputElement>({
        { "POSITION", rhi::Format::float32x3, offsetof(assets::Vertex, position) },
        { "TEXCOORD", rhi::Format::float32x2, offsetof(assets::Vertex, uv) }
    });

    constexpr static auto kSamplers = std::to_array<rhi::Sampler>({
        { rhi::ShaderVisibility::ePixel }
    });

    constexpr static auto kRanges = std::to_array<rhi::BindingRange>({
        { 0, rhi::Object::eTexture, rhi::BindingMutability::eStaticAtExecute }
    });

    constexpr static auto kBindings = std::to_array<rhi::Binding>({
        rhi::bindTable(rhi::ShaderVisibility::ePixel, kRanges)
    });
};

WorldGraph::WorldGraph(const WorldGraphInfo& info) : Graph(info.ctx) {
    auto global = addPass<GlobalPass>("global");
    auto scene = addPass<ScenePass>("scene");
    auto post = addPass<PostPass>("post");
    auto imgui = addPass<ImGuiPass>("imgui", info.update);
    auto present = addPass<PresentPass>("present");

    // post.renderTarget <= global.renderTarget
    // scene.sceneTarget <= global.sceneTarget
    link(post->renderTargetIn, global->renderTargetOut);

    // post.sceneTarget <= scene.sceneTarget
    link(post->sceneTargetIn, scene->sceneTargetOut);

    // imgui.renderTarget <= post.renderTarget
    link(imgui->renderTargetIn, post->renderTargetOut);

    // present.renderTarget <= imgui.renderTarget
    // present.sceneTarget <= scene.sceneTarget
    link(present->renderTargetIn, imgui->renderTargetOut);
    link(present->sceneTargetIn, post->sceneTargetOut);

    primary = present;
}
