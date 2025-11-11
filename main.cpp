// Standard Library Headers
#include <atomic>
#include <cmath>
#include <filesystem>
#include <mutex>
#include <thread>
#include <vector>
#include <chrono>

// NES Emulator Components
#include "NES.h"

// UI and Rendering
#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_sdlrenderer2.h"

// SDL
#include <SDL.h>
#include <SDL_log.h> // Para log no Android (logcat)

// --- CONFIGURAÇÕES PARA ANDROID ---
// Defina aqui o caminho onde o emulador procurará por ROMs.
// Crie esta pasta no seu dispositivo e coloque seus arquivos .nes aqui.
const char* ANDROID_ROM_PATH = "/sdcard/CalascioNES/roms/";
const char* DEFAULT_ROM_NAME = "default.nes"; // O emulador tentará carregar este arquivo ao iniciar.

// --- CONSTANTES E GLOBAIS ---
// A escala pode precisar de ajuste dependendo da resolução da tela do seu dispositivo.
constexpr int SCALE = 3;
constexpr int SCREEN_WIDTH = 256 * SCALE;
constexpr int SCREEN_HEIGHT = 240 * SCALE;

// Audio Buffer
constexpr int BUFFER_SIZE = 8192;
int16_t audio_buffer[BUFFER_SIZE];
uint16_t write_pos = 0;
uint16_t read_pos = 0;

// Estado do Emulador
std::atomic<bool> running(true);
std::mutex framebuffer_mutex;
std::vector<uint32_t> screen(256 * 240, 0);

// Estado dos controles, agora atualizado por toque
// Bitmask: 0=A, 1=B, 2=Select, 3=Start, 4=Up, 5=Down, 6=Left, 7=Right
uint16_t controller_state = 0;

// Texturas SDL2
SDL_Texture* screenBuffer = nullptr;

// Janela e Renderer Principais
SDL_Window* window = nullptr;
SDL_Renderer* renderer = nullptr;
SDL_AudioDeviceID audio_device = 0;

// Gerenciamento de FPS
double desired_fps = 60.0;
std::atomic<double> frame_time = 1000.0 / desired_fps;
int FPS;
int padding = 0; // Altura da barra de menu ImGui

// --- ESTRUTURA PARA CONTROLES DE TOQUE ---
struct VirtualButton {
    SDL_Rect rect;      // Posição e tamanho na tela
    uint16_t nes_bit;   // Bit correspondente no registrador do controle NES (0-7)
    int fingerId = -1;  // ID do dedo que está pressionando este botão
};

// Posições dos botões virtuais (ajuste conforme necessário)
VirtualButton virtual_controller[] = {
    // D-Pad (Esquerda da tela)
    { {50, SCREEN_HEIGHT - 200, 70, 70}, (1 << 6) }, // Esquerda
    { {190, SCREEN_HEIGHT - 200, 70, 70}, (1 << 7) }, // Direita
    { {120, SCREEN_HEIGHT - 270, 70, 70}, (1 << 4) }, // Cima
    { {120, SCREEN_HEIGHT - 130, 70, 70}, (1 << 5) }, // Baixo
    // Botões de Ação (Direita da tela)
    { {SCREEN_WIDTH - 120, SCREEN_HEIGHT - 150, 80, 80}, (1 << 0) }, // A
    { {SCREEN_WIDTH - 220, SCREEN_HEIGHT - 150, 80, 80}, (1 << 1) }, // B
    // Start/Select (Centro)
    { {SCREEN_WIDTH/2 + 10, SCREEN_HEIGHT - 60, 100, 40}, (1 << 3) }, // Start
    { {SCREEN_WIDTH/2 - 110, SCREEN_HEIGHT - 60, 100, 40}, (1 << 2) }  // Select
};

// --- DECLARAÇÕES DE FUNÇÕES ---
void audio_callback(void* userdata, Uint8* stream, int len);
void emulate_nes(NES* nes);
void initImGui();
void cleanupImGui();
void handle_events(NES* nes);
void handle_imGui(NES* nes);
void draw_frame(std::shared_ptr<PPU> ppu);
void draw_touch_controls();
void update_controller_state(Bus* bus);

// --- PONTO DE ENTRADA PRINCIPAL (SDL_main) ---
int SDL_main(int argc, char* argv[]) {
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) < 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Couldn't initialize SDL: %s", SDL_GetError());
        return 1;
    }

    window = SDL_CreateWindow("CalascioNES", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, SCREEN_WIDTH, SCREEN_HEIGHT, SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!window) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to create window: %s", SDL_GetError());
        return 1;
    }

    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to create renderer: %s", SDL_GetError());
        return 1;
    }

    SDL_AudioSpec desired_spec{};
    desired_spec.freq = 44100;
    desired_spec.format = AUDIO_S16SYS;
    desired_spec.channels = 1;
    desired_spec.samples = 1024;
    desired_spec.callback = audio_callback;
    audio_device = SDL_OpenAudioDevice(NULL, 0, &desired_spec, NULL, 0);
    if (audio_device == 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to open audio: %s", SDL_GetError());
    } else {
        SDL_PauseAudioDevice(audio_device, 0);
    }

    initImGui();
    screenBuffer = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_STREAMING, 256, 240);

    NES nes;
    nes.set_audio_buffer(audio_buffer, BUFFER_SIZE, &write_pos);

    // Tenta carregar uma ROM padrão no início
    std::string default_rom = std::string(ANDROID_ROM_PATH) + DEFAULT_ROM_NAME;
    if (std::filesystem::exists(default_rom)) {
        if(nes.load_game(default_rom)) {
             SDL_SetWindowTitle(window, ("CalascioNES" + nes.get_info()).c_str());
        }
    } else {
        SDL_Log("Default ROM not found at %s", default_rom.c_str());
        SDL_Log("Please create the folder and place a ROM named 'default.nes' inside.");
    }

    std::thread emulation_thread(emulate_nes, &nes);

    // Medir altura da barra de menu uma vez
    ImGui_ImplSDLRenderer2_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();
    if (ImGui::BeginMainMenuBar()) {
        padding = ImGui::GetFrameHeight();
        ImGui::EndMainMenuBar();
    }
    ImGui::Render();

    while (running) {
        handle_events(&nes);

        // Renderiza o frame do emulador
        draw_frame(nes.get_ppu());

        // Renderiza a UI (ImGui e controles de toque)
        ImGui_ImplSDLRenderer2_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        handle_imGui(&nes);
        draw_touch_controls();

        ImGui::Render();
        ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), renderer);

        SDL_RenderPresent(renderer);
    }

    emulation_thread.join();
    cleanupImGui();
    SDL_DestroyTexture(screenBuffer);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}

void emulate_nes(NES* nes) {
    using namespace std::chrono;
    auto last_time = high_resolution_clock::now();
    int frame_count = 0;

    while (running) {
        auto frame_time_ms = duration<double, std::milli>(frame_time);
        auto frame_start = high_resolution_clock::now();

        if (nes->is_game_loaded()) {
            nes->run_frame();
        }

        {
            std::lock_guard<std::mutex> lock(framebuffer_mutex);
            screen = nes->get_ppu()->get_screen();
        }

        frame_count++;
        auto current_time = high_resolution_clock::now();
        if (duration<double>(current_time - last_time).count() >= 1.0) {
            FPS = frame_count;
            frame_count = 0;
            last_time = current_time;
        }

        auto frame_end = high_resolution_clock::now();
        auto elapsed = duration<double, std::milli>(frame_end - frame_start);
        auto remaining_time = frame_time_ms - elapsed;

        if (remaining_time.count() > 1) {
            SDL_Delay(static_cast<Uint32>(remaining_time.count() - 1));
        }
    }
}

void audio_callback(void* userdata, Uint8* stream, int len) {
    int16_t* output = reinterpret_cast<int16_t*>(stream);
    int samples_needed = len / sizeof(int16_t);
    int available_data = (write_pos >= read_pos) ? (write_pos - read_pos) : ((BUFFER_SIZE - read_pos) + write_pos);

    if (available_data >= samples_needed)
    {
        if (read_pos + samples_needed > BUFFER_SIZE)
        {
            int first_chunk_size = BUFFER_SIZE - read_pos;
            std::copy(audio_buffer + read_pos, audio_buffer + BUFFER_SIZE, output);
            std::copy(audio_buffer, audio_buffer + (samples_needed - first_chunk_size), output + first_chunk_size);
        }
        else
            std::copy(audio_buffer + read_pos, audio_buffer + read_pos + samples_needed, output);

        read_pos = (read_pos + samples_needed) & (BUFFER_SIZE-1);
    } else {
         std::fill(output, output + samples_needed, 0);
    }
}

void draw_frame(std::shared_ptr<PPU> ppu) {
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);
    SDL_Rect screen_rect = {0, padding, SCREEN_WIDTH, SCREEN_HEIGHT};
    {
        std::lock_guard<std::mutex> lock(framebuffer_mutex);
        SDL_UpdateTexture(screenBuffer, NULL, screen.data(), 256 * 4);
        SDL_RenderCopy(renderer, screenBuffer, NULL, &screen_rect);
    }
}

void draw_touch_controls() {
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 128, 128, 128, 100); // Cinza semi-transparente
    for (const auto& button : virtual_controller) {
        SDL_RenderFillRect(renderer, &button.rect);
        if (button.fingerId != -1) { // Destaque se pressionado
            SDL_SetRenderDrawColor(renderer, 255, 255, 255, 150);
            SDL_RenderFillRect(renderer, &button.rect);
            SDL_SetRenderDrawColor(renderer, 128, 128, 128, 100);
        }
    }
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
}

void update_controller_state(Bus* bus) {
    // A implementação original usa um strobe e um shift register.
    // Para simplificar com controles de toque, podemos escrever o estado diretamente.
    // O jogo lerá o estado mais recente quando o strobe for ativado.
    bus->cpu_writes(0x4016, 1); // Strobe ON para capturar o estado
    bus->cpu_writes(0x4016, 0); // Strobe OFF para permitir a leitura serial
    // A lógica em Bus.cpp precisa ser compatível com este método
    // A maneira mais fácil é modificar Bus::cpu_writes para armazenar o estado
    // quando o strobe é ativado.
}

void handle_events(NES* nes) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        ImGui_ImplSDL2_ProcessEvent(&event);

        switch (event.type) {
            case SDL_QUIT:
                running = false;
                break;

            // Gerenciamento do ciclo de vida do App no Android
            case SDL_APP_WILLENTERBACKGROUND:
                if (nes->is_game_loaded()) nes->change_pause(audio_device);
                break;
            case SDL_APP_DIDENTERFOREGROUND:
                if (nes->is_game_loaded()) nes->change_pause(audio_device);
                break;

            // Gerenciamento de toques na tela
            case SDL_FINGERDOWN:
            case SDL_FINGERUP:
            {
                bool touch_on_button = false;
                int window_w, window_h;
                SDL_GetWindowSize(window, &window_w, &window_h);
                SDL_Point touch_point = { (int)(event.tfinger.x * window_w), (int)(event.tfinger.y * window_h) };

                for (auto& button : virtual_controller) {
                    if (SDL_PointInRect(&touch_point, &button.rect)) {
                        touch_on_button = true;
                        if (event.type == SDL_FINGERDOWN) {
                            if (button.fingerId == -1) { // Aceita o primeiro dedo
                                button.fingerId = event.tfinger.fingerId;
                                controller_state |= button.nes_bit;
                            }
                        } else { // FINGERUP
                            if (button.fingerId == event.tfinger.fingerId) {
                                button.fingerId = -1;
                                controller_state &= ~button.nes_bit;
                            }
                        }
                    } else if (event.type == SDL_FINGERUP && button.fingerId == event.tfinger.fingerId) {
                        // Dedo foi arrastado para fora e solto
                        button.fingerId = -1;
                        controller_state &= ~button.nes_bit;
                    }
                }
                
                // Lógica do Zapper: um toque na tela do jogo
                SDL_Rect game_screen_rect = {0, padding, window_w, window_h - padding};
                if (!touch_on_button && nes->get_zapper() && SDL_PointInRect(&touch_point, &game_screen_rect)) {
                     if (event.type == SDL_FINGERDOWN) {
                        nes->send_mouse_coordinates((touch_point.x * 256) / window_w, ((touch_point.y - padding) * 240) / (window_h - padding));
                     } else { // FINGERUP
                        nes->fire_zapper();
                     }
                }
                
                // Escreve o estado para o BUS. A lógica no Bus.cpp já deve lidar com isso.
                // A escrita para 0x4016 no bus deve atualizar o estado interno do shift_register
                // A maneira como Bus.cpp está escrito é um pouco complexa, mas vamos confiar nela.
                // A parte importante é que o estado seja capturado quando strobe=1.
                // Aqui, vamos apenas garantir que o estado esteja disponível.
                // Bus.cpp precisa ser modificado para usar `controller_state`
                // Em Bus.cpp, na função cpu_writes, quando address == 0x4016:
                // if (strobe) { shift_register_controller1 = controller_state & 0xFF; }
                 nes->get_bus()->cpu_writes(0x4016, 1);
                 nes->get_bus()->cpu_writes(0x4016, 0);


                break;
            }
        }
    }
     // Fora do loop de eventos, atualizamos o estado do controle no bus
     // Acessar o estado do teclado é a maneira antiga, vamos usar a variável global.
     const uint8_t *keystate = SDL_GetKeyboardState(NULL);
     if (keystate[SDL_SCANCODE_P]) { /* do nothing, handled by menu */ }
     // A escrita em 0x4016 no Bus.cpp é o que realmente atualiza o estado
     // Mas ela depende do keystate. Vamos precisar modificar o Bus.cpp.
}


void initImGui() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    ImGui_ImplSDL2_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer2_Init(renderer);
    ImGui::StyleColorsLight();
}

void cleanupImGui() {
    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
}

void handle_imGui(NES* nes) {
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Open ROM", nullptr, false, false)) {
                // Desabilitado no Android por simplicidade
            }
            if (ImGui::MenuItem("Exit")) {
                running = false;
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Game")) {
            if (ImGui::MenuItem("Pause")) {
                if (nes->is_game_loaded()) nes->change_pause(audio_device);
            }
            if (ImGui::MenuItem("Reset")) {
                if (nes->is_game_loaded()) nes->reload_game();
            }
            ImGui::EndMenu();
        }
        if(ImGui::BeginMenu("Settings")) {
            if (ImGui::MenuItem("Toggle Zapper")) {
                nes->alternate_zapper();
            }
            ImGui::EndMenu();
        }

        ImGui::SameLine(ImGui::GetWindowWidth() - 80);
        ImGui::Text("FPS: %d", FPS);

        ImGui::EndMainMenuBar();
    }
}

// Uma última modificação necessária é no arquivo Bus.cpp.
// A lógica de leitura do teclado deve ser substituída pela variável global `controller_state`.
// Em `Bus::cpu_writes`, altere a seção `if (address == 0x4016)` para:
/*
// Em Bus.cpp
extern uint16_t controller_state; // Adicione esta declaração no topo do arquivo

void Bus::cpu_writes(uint16_t address, uint8_t value)
{
    // ...
    else if ((address >= 0x4000) && (address < 0x4018))
    {
        if (address == 0x4016)
        {
            strobe = value & 1;
            if (strobe)
            {
                // Em vez de ler o teclado, usamos o estado global atualizado pelo toque
                shift_register_controller1 = controller_state & 0xFF;
                // A lógica para o controller 2 pode ser mantida ou removida
            }
        }
        else
            apu->cpu_writes(address, value);
    }
    // ...
}
*/
