/*
  NOTE(yigit): The debug UI - every Dear ImGui window the game shows.

  This is where widgets are made.  Each call IS a widget: ImGui::Checkbox
  draws the box this frame and returns true on the frame it is clicked -
  there is nothing to create or keep.  Add a line here to add a control, or
  a new function like DrawDebugUI for a new window, and call it from
  GameUpdateAndRender between ImGuiBeginFrame and ImGui::Render.

  The plumbing - starting ImGui, its input, its frame - is game_imgui.cpp.
  To see what widgets there are, tick "Dear ImGui demo window": every
  section of it is made by the code in external/imgui/imgui_demo.cpp.

  Only usable in cursor mode (Tab) - in camera mode the mouse turns the
  camera, and ImGui is not given it.
*/

// game_state, dx12_renderer and imgui.h.  game.cpp has already included it
// before us, so in the build the include guard skips it - it is here so
// this file makes sense on its own, to a reader and to the editor.
#include "dx12_renderer.cpp"

// The "clash" window: frame time, the GPU, and a few live settings.
internal void
DrawDebugUI(game_state *State, dx12_renderer *Renderer)
{
    // Always as big as what is in it - so adding a line here never leaves
    // it hidden behind a scroll bar in a size remembered from imgui.ini.
    ImGui::SetNextWindowPos(ImVec2(10.0f, 10.0f), ImGuiCond_FirstUseEver);
    ImGui::Begin("clash", 0, ImGuiWindowFlags_AlwaysAutoResize);

    ImGuiIO &IO = ImGui::GetIO();
    ImGui::Text("%.2f ms  (%.0f fps)", 1000.0f / IO.Framerate, IO.Framerate);
    ImGui::Text("%s", Renderer->GpuName);
    ImGui::Text("feature level %d_%d, ray tracing %s, mesh shaders %s",
                (Renderer->FeatureLevel >> 12) & 0xF, (Renderer->FeatureLevel >> 8) & 0xF,
                Renderer->SupportsRaytracing ? "yes" : "no",
                Renderer->SupportsMeshShaders ? "yes" : "no");

    // Video memory - see RendererGetVideoMemory for what the numbers mean.
    // Cheap enough to ask every frame.
    DXGI_QUERY_VIDEO_MEMORY_INFO Local, NonLocal;
    if(ImGui::CollapsingHeader("video memory", ImGuiTreeNodeFlags_DefaultOpen) &&
       RendererGetVideoMemory(Renderer, &Local, &NonLocal))
    {
        r32 ToMB = 1.0f / (1024.0f*1024.0f);

        // The bar is this process's VRAM against its budget - near the end,
        // and loading more starts to stutter.
        r32 Used = (Local.Budget > 0) ? ((r32)Local.CurrentUsage / (r32)Local.Budget) : 0.0f;
        char Overlay[64];
        snprintf(Overlay, sizeof(Overlay), "%.0f / %.0f MB",
                 (r32)Local.CurrentUsage*ToMB, (r32)Local.Budget*ToMB);
        ImGui::ProgressBar(Used, ImVec2(-1.0f, 0.0f), Overlay);

        ImGui::Text("VRAM budget %.0f MB of %.0f MB on the card",
                    (r32)Local.Budget*ToMB, (r32)Renderer->DedicatedVideoMemory*ToMB);
        ImGui::Text("shared (system RAM) %.0f / %.0f MB",
                    (r32)NonLocal.CurrentUsage*ToMB, (r32)NonLocal.Budget*ToMB);
    }

    ImGui::Separator();
    ImGui::ColorEdit3("clear colour", Renderer->ClearColor);

    // ImGui wants a bool; game_state keeps b32s.
    bool SoundOn = (State->SoundOn != 0);
    if(ImGui::Checkbox("sound", &SoundOn))
    {
        State->SoundOn = SoundOn;
    }

    ImGui::SliderFloat("camera speed", &State->Camera.MovementSpeed,
                       CAMERA_MIN_SPEED, CAMERA_MAX_SPEED);
    ImGui::Text("camera  %.2f %.2f %.2f   yaw %.1f  pitch %.1f",
                State->Camera.Position.X, State->Camera.Position.Y, State->Camera.Position.Z,
                State->Camera.Yaw * (180.0f / Pi32), State->Camera.Pitch * (180.0f / Pi32));

    ImGui::Separator();

    if(ImGui::CollapsingHeader("lights"))
    {
        ImGui::SliderFloat3("sun", &Renderer->LightStrengths[0].x, 0.0f, 1.0f);
        ImGui::SliderFloat3("fill", &Renderer->LightStrengths[1].x, 0.0f, 1.0f);
        ImGui::SliderFloat3("back", &Renderer->LightStrengths[2].x, 0.0f, 1.0f);
        ImGui::SliderFloat3("ambient", &Renderer->AmbientLight.x, 0.0f, 1.0f);
    }

    // Each material's roughness.  The slider edits the CPU copy only - so
    // when it changes, the material is marked dirty, and UpdateMaterialBuffer
    // copies it into each frame resource's material buffer in turn.
    if(ImGui::CollapsingHeader("materials"))
    {
        // In material_id order.
        const char *MaterialNames[MATERIAL_COUNT] =
        {
            "forest green", "steel blue", "light gray", "circle", "lake blue",
        };
        for(u32 MaterialIndex = 0; MaterialIndex < MATERIAL_COUNT; ++MaterialIndex)
        {
            material *Material = &Renderer->Materials[MaterialIndex];
            if(ImGui::SliderFloat(MaterialNames[MaterialIndex], &Material->Roughness, 0.0f, 1.0f))
            {
                Material->NumFramesDirty = FRAME_RESOURCE_COUNT;
            }
        }
    }

    ImGui::Separator();
    bool ShowDemo = (State->ShowImGuiDemo != 0);
    if(ImGui::Checkbox("Dear ImGui demo window", &ShowDemo))
    {
        State->ShowImGuiDemo = ShowDemo;
    }
    ImGui::TextDisabled("Tab frees the cursor to use these.");

    ImGui::End();

    if(State->ShowImGuiDemo)
    {
        bool Open = true;
        ImGui::ShowDemoWindow(&Open);
        State->ShowImGuiDemo = Open;
    }
}
