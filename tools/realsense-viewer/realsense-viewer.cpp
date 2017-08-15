#include <librealsense/rs2.hpp>
#include "model-views.h"

#include <cstdarg>
#include <thread>
#include <iostream>
#include <algorithm>
#include <iomanip>
#include <map>
#include <sstream>
#include <array>
#include <mutex>
#include <set>

#include <imgui_internal.h>

#include <noc_file_dialog.h>

#pragma comment(lib, "opengl32.lib")

using namespace rs2;
using namespace rs400;


struct user_data
{
    GLFWwindow* curr_window = nullptr;
    mouse_info* mouse = nullptr;
    context ctx;
    viewer_model* model = nullptr;
};

int main(int, char**) try
{
    // Init GUI
    if (!glfwInit()) exit(1);

    rs2_error* e = nullptr;
    std::string title = to_string() << "RealSense Viewer v" << api_version_to_string(rs2_get_api_version(&e));

    // Create GUI Windows
    auto window = glfwCreateWindow(1280, 720, title.c_str(), nullptr, nullptr);
    glfwMakeContextCurrent(window);
    ImGui_ImplGlfw_Init(window, true);

    ImFont *font_18, *font_14;
    imgui_easy_theming(font_14, font_18);

    // Create RealSense Context
    context ctx;
    auto refresh_device_list = true;
    std::vector<std::string> restarting_device_info;

    bool is_3d_view = false;
    bool anything_started = false;

    std::vector<std::pair<std::string, std::string>> device_names;

    std::string error_message{ "" };
    std::string label{ "" };

    auto last_time_point = std::chrono::high_resolution_clock::now();

    std::vector<device_model> device_models;
    device_model* device_to_remove = nullptr;

    viewer_model viewer_model(ctx);
    device_list list;
    bool paused = false;

    std::vector<device> devs;
    std::mutex m;

    mouse_info mouse;

    user_data data;
    data.curr_window = window;
    data.mouse = &mouse;
    data.ctx = ctx;
    data.model = &viewer_model;

    glfwSetWindowUserPointer(window, &data);

    glfwSetCursorPosCallback(window, [](GLFWwindow* w, double cx, double cy)
    {
        auto data = reinterpret_cast<user_data*>(glfwGetWindowUserPointer(w));
        data->mouse->cursor = { (float)cx, (float)cy };
    });
    glfwSetMouseButtonCallback(window, [](GLFWwindow* w, int button, int action, int mods)
    {
        auto data = reinterpret_cast<user_data*>(glfwGetWindowUserPointer(w));
        data->mouse->mouse_down = (button == GLFW_MOUSE_BUTTON_1) && (action != GLFW_RELEASE);
    });
    glfwSetScrollCallback(window, [](GLFWwindow * w, double xoffset, double yoffset)
    {
        auto data = reinterpret_cast<user_data*>(glfwGetWindowUserPointer(w));
        data->mouse->mouse_wheel = yoffset;
        data->mouse->ui_wheel += yoffset;
    });

    // TODO: Implement same logic as when doing this from GUI
    //glfwSetDropCallback(window, [](GLFWwindow* w, int count, const char** paths)
    //{
    //    auto data = reinterpret_cast<user_data*>(glfwGetWindowUserPointer(w));

    //    if (count <= 0)
    //        return;

    //    for (int i = 0; i < count; i++)
    //    {
    //        try
    //        {
    //            data->ctx.load_device(paths[i]);
    //        }
    //        catch (...)
    //        {
    //            data->model->not_model.add_notification({ to_string() << "Could not load \"" << paths[i] << "\"",
    //                0, RS2_LOG_SEVERITY_ERROR, RS2_NOTIFICATION_CATEGORY_UNKNOWN_ERROR });
    //        }
    //    }
    //});

    ctx.set_devices_changed_callback([&](event_information& info)
    {
        std::lock_guard<std::mutex> lock(m);

        for (auto dev : devs)
        {
            if (info.was_removed(dev))
            {
                viewer_model.not_model.add_notification({ get_device_name(dev).first + " Disconnected\n",
                    0, RS2_LOG_SEVERITY_INFO, RS2_NOTIFICATION_CATEGORY_UNKNOWN_ERROR });
            }
        }

        try
        {
            for (auto dev : info.get_new_devices())
            {
                viewer_model.not_model.add_notification({ get_device_name(dev).first + " Connected\n",
                    0, RS2_LOG_SEVERITY_INFO, RS2_NOTIFICATION_CATEGORY_UNKNOWN_ERROR });
            }
        }
        catch (...)
        {

        }
        refresh_device_list = true;
    });

    // Closing the window
    while (!glfwWindowShouldClose(window))
    {
        {
            std::lock_guard<std::mutex> lock(m);

            if (refresh_device_list)
            {
                refresh_device_list = false;

                try
                {
                    auto prev_size = list.size();
                    list = ctx.query_devices();

                    device_names = get_devices_names(list);

                    if (device_models.size() == 0 && list.size() > 0 && prev_size == 0)
                    {
                        auto dev = [&](){
                            for (size_t i = 0; i < list.size(); i++)
                            {
                                if (list[i].supports(RS2_CAMERA_INFO_NAME) &&
                                    std::string(list[i].get_info(RS2_CAMERA_INFO_NAME)) != "Platform Camera")
                                    return list[i];
                            }
                            return device();
                        }();

                        if (dev)
                        {
                            auto model = device_model(dev, error_message);
                            device_models.push_back(model);
                            viewer_model.not_model.add_log(to_string() << model.dev.get_info(RS2_CAMERA_INFO_NAME) << " was selected as a default device");
                        }
                    }

                    devs.clear();
                    for (auto&& sub : list)
                    {
                        devs.push_back(sub);
                        for (auto&& s : sub.query_sensors())
                        {
                            s.set_notifications_callback([&](const notification& n)
                            {
                                viewer_model.not_model.add_notification({ n.get_description(), n.get_timestamp(), n.get_severity(), n.get_category() });
                            });
                        }
                    }


                    device_to_remove = nullptr;
                    while(true)
                    {
                        for (auto&& dev_model : device_models)
                        {
                            bool still_around = false;
                            for (auto&& dev : devs)
                                if (get_device_name(dev_model.dev) == get_device_name(dev))
                                    still_around = true;
                            if (!still_around) {
                                for (auto&& s : dev_model.subdevices)
                                    s->streaming = false;
                                device_to_remove = &dev_model;
                            }
                        }
                        if (device_to_remove)
                        {
                            device_models.erase(std::find_if(begin(device_models), end(device_models),
                                [&](const device_model& other) { return get_device_name(other.dev) == get_device_name(device_to_remove->dev); }));
                            device_to_remove = nullptr;
                        }
                        else break;
                    }
                }
                catch (const error& e)
                {
                    error_message = error_to_string(e);
                }
                catch (const std::exception& e)
                {
                    error_message = e.what();
                }
            }
        }

        bool update_read_only_options = false;
        auto now = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration<double, std::milli>(now - last_time_point).count();
        
        if (duration >= 6000)
        {
            update_read_only_options = true;
            last_time_point = now;
        }

        glfwPollEvents();
        int w, h;
        glfwGetWindowSize(window, &w, &h);


        const float panel_width = 320.f;
        const float panel_y = 44.f;
        const float default_log_h = 80.f;

        auto output_height = (viewer_model.is_output_collapsed ? default_log_h : 20);

        rect viewer_rect = { panel_width, panel_y, w - panel_width, h - panel_y - output_height };

        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();

        ImGui::GetIO().MouseWheel = mouse.ui_wheel;
        mouse.ui_wheel = 0.f;

        ImGui_ImplGlfw_NewFrame();

        // Flags for pop-up window - no window resize, move or collaps
        auto flags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar |
            ImGuiWindowFlags_NoSavedSettings;

        ImGui::SetNextWindowPos({ 0, 0 });
        ImGui::SetNextWindowSize({ panel_width, panel_y });

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::Begin("Add Device Panel", nullptr, flags);

        ImGui::PushFont(font_18);
        ImGui::PushStyleColor(ImGuiCol_PopupBg, from_rgba(230, 230, 230, 255));
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, from_rgba(0, 0xae, 0xff, 255));
        ImGui::PushStyleColor(ImGuiCol_TextSelectedBg, from_rgba(255, 255, 255, 255));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(5, 5));
        ImGui::SetNextWindowPos({ 0, panel_y });

        if (ImGui::Button(u8"Add Source\t\t\t\t\t\t\t\t\t\t\t\t\uf0d7", { panel_width - 1, panel_y }))
            ImGui::OpenPopup("select");

        auto new_devices_count = device_names.size() + 1;
        for (auto&& dev_model : device_models)
            if (list.contains(dev_model.dev) || dev_model.dev.is<playback>())
                new_devices_count--;

        ImGui::PushFont(font_14);
        ImGui::SetNextWindowSize({ panel_width, 20.f * new_devices_count + 8 });
        if (ImGui::BeginPopup("select"))
        {
            ImGui::PushStyleColor(ImGuiCol_Text, dark_grey);
            ImGui::Columns(2, "DevicesList", false);
            for (size_t i = 0; i < device_names.size(); i++)
            {
                bool skip = false;
                for (auto&& dev_model : device_models)
                    if (get_device_name(dev_model.dev) == device_names[i]) skip = true;
                if (skip) continue;

                if (ImGui::Selectable(device_names[i].first.c_str(), false, ImGuiSelectableFlags_SpanAllColumns)/* || switch_to_newly_loaded_device*/)
                {
                    try
                    {
                        auto dev = list[i];
                        auto model = device_model(dev, error_message);
                        device_models.push_back(model);
                    }
                    catch (const error& e)
                    {
                        error_message = error_to_string(e);
                    }
                    catch (const std::exception& e)
                    {
                        error_message = e.what();
                    }
                }

                if (ImGui::IsItemHovered())
                {
                    ImGui::PushStyleColor(ImGuiCol_Text, from_rgba(255, 255, 255, 255));
                    ImGui::NextColumn();
                    ImGui::Text("S/N: %s", device_names[i].second.c_str());
                    ImGui::NextColumn();
                    ImGui::PopStyleColor();
                }
                else
                {
                    ImGui::NextColumn();
                    ImGui::Text("S/N: %s", device_names[i].second.c_str());
                    ImGui::NextColumn();
                }

            }

            if (new_devices_count > 1) ImGui::Separator();

            if (ImGui::Selectable("Load Recorded Sequence", false, ImGuiSelectableFlags_SpanAllColumns))
            {
                const char *ret;
                ret = noc_file_dialog_open(NOC_FILE_DIALOG_OPEN,
                    "ROS-bag\0*.bag\0", NULL, NULL);
                if (ret)
                {
                    try
                    {
                        auto dev = ctx.load_device(ret);
                        auto model = device_model(dev, error_message);
                        device_models.push_back(model);

                        if (auto p = dev.as<playback>())
                        {
                            auto filename = p.file_name();
                            p.set_status_changed_callback([&device_models, filename](rs2_playback_status status)
                            {
                                if (status == RS2_PLAYBACK_STATUS_STOPPED)
                                {
                                    auto it = std::find_if(device_models.begin(), device_models.end(), 
                                        [&](const device_model& dm) {
                                        if (auto p = dm.dev.as<playback>())
                                            return p.file_name() == filename;
                                        return false;
                                    });
                                    if (it != device_models.end())
                                    {
                                        for (auto&& sub : it->subdevices)
                                        {
                                            sub->stop();
                                        }
                                    }
                                }
                            });
                        }
                    }
                    catch (const error& e)
                    {
                        error_message = error_to_string(e);
                    }
                    catch (const std::exception& e)
                    {
                        error_message = e.what();
                    }
                }
            }
            ImGui::NextColumn();
            ImGui::Text("");
            ImGui::NextColumn();

            ImGui::PopStyleColor();
            ImGui::EndPopup();
        }
        ImGui::PopFont();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
        ImGui::PopStyleColor();
        ImGui::PopStyleColor();
        ImGui::PopFont();

        ImGui::End();
        ImGui::PopStyleVar();


        ImGui::SetNextWindowPos({ panel_width, 0 });
        ImGui::SetNextWindowSize({ w - panel_width, panel_y });

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::PushStyleColor(ImGuiCol_WindowBg, from_rgba(0x2d, 0x37, 0x40, 0xff));
        ImGui::Begin("Toolbar Panel", nullptr, flags);

        ImGui::PushStyleColor(ImGuiCol_Border, black);
        ImGui::SetCursorPosX(w - panel_width - panel_y * 2);
        ImGui::PushStyleColor(ImGuiCol_Text, is_3d_view ? grey : white);
        ImGui::PushStyleColor(ImGuiCol_TextSelectedBg, is_3d_view ? grey : white);
        if (ImGui::Button("2D", { panel_y,panel_y })) is_3d_view = false;
        ImGui::PopStyleColor(2);
        ImGui::SameLine();
        ImGui::SetCursorPosX(w - panel_width - panel_y * 1);
        ImGui::PushStyleColor(ImGuiCol_Text, !is_3d_view ? grey : white);
        ImGui::PushStyleColor(ImGuiCol_TextSelectedBg, !is_3d_view ? grey : white);
        if (ImGui::Button("3D", { panel_y,panel_y }))
        {
            is_3d_view = true;
            viewer_model.update_3d_camera(viewer_rect, mouse, true);
        }
        ImGui::PopStyleColor(3);

        ImGui::SameLine();
        ImGui::SetCursorPosX(w - panel_width - panel_y);

        ImGui::PushStyleColor(ImGuiCol_TextSelectedBg, white);
        ImGui::PushStyleColor(ImGuiCol_PopupBg, almost_white_bg);
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, light_blue);
        ImGui::PushStyleColor(ImGuiCol_TextSelectedBg, white);

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(5, 5));

        //if (ImGui::Button(u8"\uf013\uf0d7", { panel_y,panel_y }))
        //    ImGui::OpenPopup("global_menu");

        //ImGui::PushFont(font_14);
        //if (ImGui::BeginPopup("global_menu"))
        //{
        //    ImGui::PushStyleColor(ImGuiCol_Text, dark_grey);
        //    if (ImGui::Selectable("About RealSense Viewer"))
        //    {
        //    }

        //    ImGui::PopStyleColor();
        //    ImGui::EndPopup();
        //}
        //ImGui::PopFont();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(4);

        ImGui::End();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();

        viewer_model.show_event_log(font_14, panel_width,
            h - (viewer_model.is_output_collapsed ? default_log_h : 20),
            w - panel_width, default_log_h);

        // Set window position and size
        ImGui::SetNextWindowPos({ 0, panel_y });
        ImGui::SetNextWindowSize({ panel_width, h - panel_y });
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::PushStyleColor(ImGuiCol_WindowBg, from_rgba(0x1b, 0x21, 0x25, 0xff));

        // *********************
        // Creating window menus
        // *********************
        ImGui::Begin("Control Panel", nullptr, flags | ImGuiWindowFlags_AlwaysVerticalScrollbar);

        if (device_models.size() > 0)
        {
            std::map<subdevice_model*, float> model_to_y;
            std::map<subdevice_model*, float> model_to_abs_y;
            auto windows_width = ImGui::GetContentRegionMax().x;

            for (auto&& dev_model : device_models)
            {
                auto header_h = panel_y;
                if (dev_model.dev.is<playback>()) header_h += 15;

                ImGui::PushFont(font_14);
                auto pos = ImGui::GetCursorScreenPos();
                ImGui::GetWindowDrawList()->AddRectFilled(pos, { pos.x + panel_width, pos.y + header_h }, ImColor(sensor_header_light_blue));
                ImGui::GetWindowDrawList()->AddLine({ pos.x,pos.y }, { pos.x + panel_width,pos.y }, ImColor(black));

                pos = ImGui::GetCursorPos();
                ImGui::PushStyleColor(ImGuiCol_Button, sensor_header_light_blue);
                //ImGui::Columns(2, "DeviceInfo", false);
                ImGui::SetCursorPos({ 8, pos.y + 14 });
                if (dev_model.is_recording)
                {
                    ImGui::PushStyleColor(ImGuiCol_Text, redish);
                    label = to_string() << u8"\uf111";
                    ImGui::Text(label.c_str());
                    ImGui::PopStyleColor();
                }
                else if (dev_model.dev.is<playback>())
                {
                    label = to_string() << u8" \uf008";
                    ImGui::Text(label.c_str());
                }
                else
                {
                    label = to_string() << u8" \uf03d";
                    ImGui::Text(label.c_str());
                }
                ImGui::SameLine();

                label = to_string() << dev_model.dev.get_info(RS2_CAMERA_INFO_NAME);
                ImGui::Text(label.c_str());
                //ImGui::NextColumn();
//                ImGui::SetCursorPos({ ImGui::GetCursorPosX(), pos.y + 14 });
//                label = to_string() << "S/N: " << (dev_model.dev.supports(RS2_CAMERA_INFO_SERIAL_NUMBER) ? dev_model.dev.get_info(RS2_CAMERA_INFO_SERIAL_NUMBER) : "Unknown");
//                ImGui::Text(label.c_str());

                ImGui::Columns(1);
                ImGui::SetCursorPos({ panel_width - 50, pos.y + 5 + (header_h - panel_y) / 2 });

                ImGui::PushStyleColor(ImGuiCol_TextSelectedBg, white);
                ImGui::PushStyleColor(ImGuiCol_PopupBg, almost_white_bg);
                ImGui::PushStyleColor(ImGuiCol_HeaderHovered, from_rgba(0, 0xae, 0xff, 255));
                ImGui::PushStyleColor(ImGuiCol_TextSelectedBg, white);

                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(5, 5));

                ImGui::PushFont(font_18);
                label = to_string() << "device_menu" << dev_model.id;
                std::string settings_button_name = to_string() << u8"\uf0c9##" << dev_model.id;

                if (ImGui::Button(settings_button_name.c_str(), { 33,35 }))
                    ImGui::OpenPopup(label.c_str());
                ImGui::PopFont();

                ImGui::PushFont(font_14);

                if (ImGui::BeginPopup(label.c_str()))
                {
                    ImGui::PushStyleColor(ImGuiCol_Text, dark_grey);
                    if (ImGui::Selectable("Show Device Details..."))
                    {

                    }

                    if (!dev_model.is_recording &&
                        !dev_model.dev.is<playback>() &&
                        ImGui::Selectable("Record to File..."))
                    {
                        auto ret = noc_file_dialog_open(NOC_FILE_DIALOG_SAVE, "ROS-bag\0*.bag\0", NULL, NULL);

                        if (ret)
                        {
                            std::string filename = ret;
                            if (!ends_with(to_lower(filename), ".bag")) filename += ".bag";

                            dev_model.start_recording(filename, error_message);
                        }
                    }

                    if (auto adv = dev_model.dev.as<advanced_mode>())
                    {
                        ImGui::Separator();

                        if (ImGui::Selectable("Load Settings", false, ImGuiSelectableFlags_SpanAllColumns))
                        {
                            auto ret = noc_file_dialog_open(NOC_FILE_DIALOG_OPEN, "JavaScript Object Notation (JSON)\0*.json\0", NULL, NULL);
                            if (ret)
                            {
                                std::ifstream t(ret);
                                std::string str((std::istreambuf_iterator<char>(t)),
                                                std::istreambuf_iterator<char>());

                                adv.load_json(str);
                                dev_model.get_curr_advanced_controls = true;
                            }
                        }

                        if (ImGui::Selectable("Save Settings", false, ImGuiSelectableFlags_SpanAllColumns))
                        {
                            auto ret = noc_file_dialog_open(NOC_FILE_DIALOG_SAVE, "JavaScript Object Notation (JSON)\0*.json\0", NULL, NULL);

                            if (ret)
                            {
                                std::string filename = ret;
                                if (!ends_with(to_lower(filename), ".json")) filename += ".json";

                                std::ofstream out(filename);
                                out << adv.serialize_json();
                                out.close();
                            }
                        }
                    }

                    ImGui::Separator();

                    if (ImGui::Selectable("Hardware Reset"))
                    {
                        try
                        {
                            restarting_device_info = get_device_info(dev_model.dev, false);
                            dev_model.dev.hardware_reset();
                        }
                        catch (const error& e)
                        {
                            error_message = error_to_string(e);
                        }
                        catch (const std::exception& e)
                        {
                            error_message = e.what();
                        }
                    }

                    ImGui::Separator();

                    if (ImGui::Selectable("Remove Source"))
                    {
                        for (auto&& sub : dev_model.subdevices)
                        {
                            if (sub->streaming)
                                sub->stop();
                        }
                        device_to_remove = &dev_model;
                    }

                    ImGui::PopStyleColor();
                    ImGui::EndPopup();
                }
                ImGui::PopFont();
                ImGui::PopStyleVar();
                ImGui::PopStyleColor(4);

                ImGui::SetCursorPos({ 33, pos.y + panel_y - 9 });
                ImGui::PushStyleColor(ImGuiCol_Text, from_rgba(0xc3, 0xd5, 0xe5, 0xff));

                int playback_control_panel_height = 0;
                if (auto p = dev_model.dev.as<playback>())
                {
                    auto full_path = p.file_name();
                    auto filename = get_file_name(full_path);

                    ImGui::Text("File: \"%s\"", filename.c_str());
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip(full_path.c_str());

                    auto playback_panel_pos = ImVec2{ 0, pos.y + panel_y + 18 };
                    ImGui::SetCursorPos(playback_panel_pos);
                    playback_panel_pos.y = dev_model.draw_playback_panel(font_14);
                    playback_control_panel_height += playback_panel_pos.y;
                }

                ImGui::SetCursorPos({ 0, pos.y + header_h + playback_control_panel_height });
                ImGui::PopStyleColor(2);
                ImGui::PopFont();


                auto sensor_top_y = ImGui::GetCursorPosY();
                ImGui::SetContentRegionWidth(windows_width - 36);

                ImGui::PushStyleColor(ImGuiCol_HeaderHovered, from_rgba(0x1b, 0x21, 0x25, 0xff));
                ImGui::PushStyleColor(ImGuiCol_Text, from_rgba(0xc3, 0xd5, 0xe5, 0xff));
                ImGui::PushFont(font_14);

                // Draw menu foreach subdevice with its properties
                for (auto&& sub : dev_model.subdevices)
                {
                    const ImVec2 pos = ImGui::GetCursorPos();
                    const ImVec2 abs_pos = ImGui::GetCursorScreenPos();
                    model_to_y[sub.get()] = pos.y;
                    model_to_abs_y[sub.get()] = abs_pos.y;
                    ImGui::GetWindowDrawList()->AddLine({ abs_pos.x, abs_pos.y - 1 },
                    { abs_pos.x + panel_width, abs_pos.y - 1 },
                        ImColor(black), 1.f);

                    label = to_string() << sub->s.get_info(RS2_CAMERA_INFO_NAME) << "##" << dev_model.id;
                    ImGui::PushStyleColor(ImGuiCol_Header, sensor_header_light_blue);

                    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, { 10, 10 });
                    ImGui::PushStyleVar(ImGuiStyleVar_ItemInnerSpacing, { 0, 0 });
                    if (ImGui::TreeNode(label.c_str()))
                    {
                        anything_started = true;
                        ImGui::PopStyleVar();
                        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, { 2, 2 });

                        sub->draw_stream_selection();


                        static const std::vector<rs2_option> drawing_order{
                            RS2_OPTION_VISUAL_PRESET,
                            RS2_OPTION_EMITTER_ENABLED,
                            RS2_OPTION_ENABLE_AUTO_EXPOSURE };

                        for (auto& opt : drawing_order)
                        {
                            if (sub->draw_option(opt, update_read_only_options, error_message, viewer_model.not_model))
                            {
                                dev_model.get_curr_advanced_controls = true;
                            }
                        }

                        label = to_string() << "Controls ##" << sub->s.get_info(RS2_CAMERA_INFO_NAME) << "," << dev_model.id;;
                        if (ImGui::TreeNode(label.c_str()))
                        {
                            for (auto i = 0; i < RS2_OPTION_COUNT; i++)
                            {
                                auto opt = static_cast<rs2_option>(i);
                                if (std::find(drawing_order.begin(), drawing_order.end(), opt) == drawing_order.end())
                                {
                                    if (sub->draw_option(opt, update_read_only_options, error_message, viewer_model.not_model))
                                    {
                                        dev_model.get_curr_advanced_controls = true;
                                    }
                                }
                            }

                            if (auto ds = sub->s.as<depth_sensor>())
                                viewer_model.draw_histogram_options(ds.get_depth_scale(), *sub);

                            ImGui::TreePop();
                        }

                        if (dev_model.dev.is<advanced_mode>() && sub->s.is<depth_sensor>())
                            dev_model.draw_advanced_mode_tab(dev_model.dev, restarting_device_info);

                        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 5);

                        ImGui::TreePop();
                    }

                    ImGui::PopStyleVar();
                    ImGui::PopStyleVar();
                    ImGui::PopStyleColor();
                }

                for (auto&& sub : dev_model.subdevices)
                {
                    sub->update(error_message, viewer_model.not_model);
                }

                ImGui::PopStyleColor(2);
                ImGui::PopFont();

                
            }

            if (device_to_remove)
            {
                if (auto p = device_to_remove->dev.as<playback>())
                {
                    ctx.unload_device(p.file_name());
                }

                device_models.erase(std::find_if(begin(device_models), end(device_models),
                    [&](const device_model& other) { return get_device_name(other.dev) == get_device_name(device_to_remove->dev); }));
                device_to_remove = nullptr;
            }

            ImGui::SetContentRegionWidth(windows_width);

            auto pos = ImGui::GetCursorScreenPos();
            auto h = ImGui::GetWindowHeight();
            if (h > pos.y - panel_y)
            {
                ImGui::GetWindowDrawList()->AddLine({ pos.x,pos.y }, { pos.x + panel_width,pos.y }, ImColor(from_rgba(0, 0, 0, 0xff)));
                ImRect bb(pos, ImVec2(pos.x + ImGui::GetContentRegionAvail().x, pos.y + ImGui::GetContentRegionAvail().y));
                ImGui::GetWindowDrawList()->AddRectFilled(bb.GetTL(), bb.GetBR(), ImColor(dark_window_background));
            }

            for (auto&& dev_model : device_models)
            {
                bool stop_recording = false;
                for (auto&& sub : dev_model.subdevices)
                {
                    try
                    {
                        static float t = 0.f;
                        t += 0.03f; // TODO: change to something more elegant

                        ImGui::SetCursorPos({ windows_width - 35, model_to_y[sub.get()] + 3 });
                        ImGui::PushFont(font_14);
                        if (sub.get() == dev_model.subdevices.begin()->get() && !anything_started)
                        {
                            ImGui::PushStyleColor(ImGuiCol_Button, from_rgba(0x1b + abs(sin(t)) * 40, 0x21 + abs(sin(t)) * 20, 0x25 + abs(sin(t)) * 30, 0xff));
                        }
                        else
                        {
                            ImGui::PushStyleColor(ImGuiCol_Button, sensor_bg);
                        }
                        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, sensor_bg);
                        ImGui::PushStyleColor(ImGuiCol_ButtonActive, sensor_bg);

                        if (!sub->streaming)
                        {
                            label = to_string() << u8"  \uf204\noff   ##" << dev_model.id << "," << sub->s.get_info(RS2_CAMERA_INFO_NAME);

                            ImGui::PushStyleColor(ImGuiCol_Text, redish);
                            ImGui::PushStyleColor(ImGuiCol_TextSelectedBg, redish + 0.1f);

                            if (sub->is_selected_combination_supported())
                            {
                                if (ImGui::Button(label.c_str(), { 30,30 }))
                                {
                                    anything_started = true;

                                    auto profiles = sub->get_selected_profiles();
                                    sub->play(profiles);

                                    for (auto&& profile : profiles)
                                    {
                                        viewer_model.streams[profile.unique_id()].dev = sub;
                                    }
                                }
                                if (ImGui::IsItemHovered())
                                {
                                    ImGui::SetTooltip("Start streaming data from this sensor");
                                    anything_started = true;
                                }
                            }
                            else
                            {
                                ImGui::TextDisabled(u8"  \uf204\noff   ");
                            }
                        }
                        else
                        {
                            label = to_string() << u8"  \uf205\n    on##" << dev_model.id << "," << sub->s.get_info(RS2_CAMERA_INFO_NAME);
                            ImGui::PushStyleColor(ImGuiCol_Text, light_blue);
                            ImGui::PushStyleColor(ImGuiCol_TextSelectedBg, light_blue + 0.1f);

                            if (ImGui::Button(label.c_str(), { 30,30 }))
                            {
                                sub->stop();

                                if (!std::any_of(dev_model.subdevices.begin(), dev_model.subdevices.end(),
                                    [](const std::shared_ptr<subdevice_model>& sm)
                                {
                                    return sm->streaming;
                                }))
                                {
                                    stop_recording = true;
                                }
                            }
                            if (ImGui::IsItemHovered())
                            {
                                ImGui::SetTooltip("Stop streaming data from selected sub-device");
                            }
                        }
                    }
                    catch (const error& e)
                    {
                        error_message = error_to_string(e);
                    }
                    catch (const std::exception& e)
                    {
                        error_message = e.what();
                    }

                    ImGui::PopStyleColor(5);
                    ImGui::PopFont();
                }

                if (dev_model.is_recording && stop_recording)
                {
                    dev_model.stop_recording();
                    for (auto&& sub : dev_model.subdevices)
                    {
                        //TODO: Fix case where sensor X recorded stream 0, then stoped it and then strated recording stream 1 (need 2 sensors for this to happen)
                        if (sub->is_selected_combination_supported())
                        {
                            auto profiles = sub->get_selected_profiles();
                            for (auto&& profile : profiles)
                            {
                                viewer_model.streams[profile.unique_id()].dev = sub;
                            }
                        }
                    }
                }
            }
            
        }
        else
        {
            const ImVec2 pos = ImGui::GetCursorScreenPos();
            ImRect bb(pos, ImVec2(pos.x + ImGui::GetContentRegionAvail().x, pos.y + ImGui::GetContentRegionAvail().y));
            ImGui::GetWindowDrawList()->AddRectFilled(bb.GetTL(), bb.GetBR(), ImColor(dark_window_background));

            viewer_model.show_no_device_overlay(font_18, 50, panel_y + 50);
        }

        ImGui::End();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();

        // Fetch frames from queues
        for (auto&& device_model : device_models)
            for (auto&& sub : device_model.subdevices)
            {
                sub->queues.foreach([&](frame_queue& queue)
                {
                    try
                    {
                        frame f;
                        if (queue.poll_for_frame(&f))
                        {
                            viewer_model.upload_frame(std::move(f));
                        }
                    }
                    catch (const error& ex)
                    {
                        error_message = error_to_string(ex);
                        sub->stop();
                    }
                    catch (const std::exception& ex)
                    {
                        error_message = ex.what();
                        sub->stop();
                    }
                });
            }

        viewer_model.gc_streams();

        // Rendering
        glViewport(0, 0,
            static_cast<int>(ImGui::GetIO().DisplaySize.x),
            static_cast<int>(ImGui::GetIO().DisplaySize.y));
        glClearColor(0,0,0,1);
        glClear(GL_COLOR_BUFFER_BIT);

        if (!is_3d_view)
        {
            glfwGetWindowSize(window, &w, &h);
            glLoadIdentity();
            glOrtho(0, w, h, 0, -1, +1);

            auto layout = viewer_model.calc_layout(panel_width, panel_y, w - panel_width, (float)h - panel_y - output_height);

            if (layout.size() == 0 && device_models.size() > 0)
            {
                viewer_model.show_no_stream_overlay(font_18, panel_width, panel_y, w, (float)h - output_height);
            }

            for (auto &&kvp : layout)
            {
                auto&& view_rect = kvp.second;
                auto stream = kvp.first;
                auto&& stream_mv = viewer_model.streams[stream];
                auto&& stream_size = stream_mv.size;
                auto stream_rect = view_rect.adjust_ratio(stream_size);

                stream_mv.show_frame(stream_rect, mouse, error_message);

                if (stream_mv.dev->is_paused())
                    viewer_model.show_paused_icon(font_18, stream_rect.x + 5, stream_rect.y + 5, stream_mv.profile.unique_id());

                stream_mv.show_stream_header(font_14, stream_rect, viewer_model);
                stream_mv.show_stream_footer(stream_rect, mouse);
            }

            // Metadata overlay windows shall be drawn after textures to preserve z-buffer functionality
            for (auto &&kvp : layout)
            {
                if (viewer_model.streams[kvp.first].metadata_displayed)
                    viewer_model.streams[kvp.first].show_metadata(mouse);
            }
        }
        else
        {
            if (paused)
                viewer_model.show_paused_icon(font_18, panel_width + 15, panel_y + 15 + 32, 0);

            viewer_model.show_3dviewer_header(font_14, viewer_rect, paused);

            viewer_model.update_3d_camera(viewer_rect, mouse);

            viewer_model.render_3d_view(viewer_rect);

        }

        if (ImGui::IsKeyPressed(' '))
        {
            if (paused)
            {
                for (auto&& s : viewer_model.streams)
                {
                    if (s.second.dev) s.second.dev->resume();
                }
            }
            else
            {
                for (auto&& s : viewer_model.streams)
                {
                    if (s.second.dev) s.second.dev->pause();
                }
            }
            paused = !paused;
        }

        viewer_model.not_model.draw(font_14, w, h);

        viewer_model.popup_if_error(font_14, error_message);

        ImGui::Render();
        glfwSwapBuffers(window);
        mouse.mouse_wheel = 0;

        // Yeild the CPU
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // Stop calculating 3D model
    viewer_model.pc.stop();

    // Stop all subdevices
    for (auto&& device_model : device_models)
        for (auto&& sub : device_model.subdevices)
        {
            if (sub->streaming)
                sub->stop();
        }

    // Cleanup
    ImGui_ImplGlfw_Shutdown();
    glfwTerminate();

    return EXIT_SUCCESS;
}
catch (const error & e)
{
    std::cerr << "RealSense error calling " << e.get_failed_function() << "(" << e.get_failed_args() << "):\n    " << e.what() << std::endl;
    return EXIT_FAILURE;
}

#ifdef WIN32
int CALLBACK WinMain(
    _In_ HINSTANCE hInstance,
    _In_ HINSTANCE hPrevInstance,
    _In_ LPSTR     lpCmdLine,
    _In_ int       nCmdShow

) {
    main(0, nullptr);
}
#endif
