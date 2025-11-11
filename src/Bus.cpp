#include "Bus.h"
#include "Cartridge.h"
#include "CPU.h"
#include "APU.h"
#include "PPU.h"

// --- MODIFICAÇÃO PARA ANDROID ---
// Declara que a variável `controller_state` é global e está definida em outro arquivo (main.cpp).
// Esta variável contém o estado dos botões virtuais pressionados na tela.
extern uint16_t controller_state;

Bus::Bus(std::shared_ptr<PPU> ppu,  std::shared_ptr<Cartridge> cart, std::shared_ptr<APU> apu, std::shared_ptr<CPU> cpu)
{
    this->cart = cart;
    this->ppu = ppu;
    this->apu = apu;
    this->cpu = cpu;
}

Bus::~Bus() {}

uint8_t Bus::cpu_reads(uint16_t address)
{
    uint8_t data = 0x00;

    if (address >= 0x2000 && address < 0x4000)
        data = ppu->cpu_reads(address & 0x7);
    
    else if (address >= 0x4000 && address < 0x4018)
    {
        if (address == 0x4016)
        {
            // A leitura quando o strobe está alto é um comportamento complexo do hardware,
            // mas a maioria dos jogos apenas lê quando o strobe está baixo.
            // A lógica aqui é serializar os bits do estado que foi capturado anteriormente.
            if (strobe)
                data = (shift_register_controller1 & 1); // Lê o estado atual do botão A
            else
            {
                // Shift out the button states bit by bit
                data = shift_register_controller1 & 1;
                shift_register_controller1 >>= 1;
            }
            data |= 0x40; // Emula bits abertos do barramento
        }

        else if (address == 0x4017)
        {
            if(zapper_connected)
            {
                data = shift_register_controller2;
                zapper.light_sensed = 1;
            }
                       
            else if (strobe)
                data = (shift_register_controller2 & 1); // Lê o estado do primeiro botão do controle 2

            else
            {
                // Shift out the button states
                data = shift_register_controller2 & 1;
                shift_register_controller2 >>= 1;
                if(zapper_connected && data == 0x00)
                {
                    zapper.trigger = 1;
                }
                
            }
            
            data |= 0x40;
        }

        else
            data = apu->cpu_reads(address);
        
    }
    
    else if (address >= 0x4020 && address <= 0xFFFF)
    {
        data = cart->cpu_reads(address);
    }

    return data;
}

void Bus::cpu_writes(uint16_t address, uint8_t value)
{
    if ((address >= 0x2000) && (address < 0x4000))
        ppu->cpu_writes((address & 0x7), value);

    else if ((address >= 0x4000) && (address < 0x4018))
    {
        if (address == 0x4016)
        {
            strobe = value & 1;
            if (strobe)
            {
                // --- MODIFICAÇÃO PARA ANDROID ---
                // A lógica original de ler o teclado foi removida.
                // Em vez disso, capturamos o estado da variável global `controller_state`,
                // que é atualizada pelos controles de toque em main.cpp.
                
                // Carrega o estado atual no registrador interno para que possa ser lido bit a bit.
                shift_register_controller1 = controller_state & 0xFF;
                
                if(!zapper_connected)
                {
                    // Se um segundo controle fosse implementado, seu estado estaria nos 8 bits superiores.
                    shift_register_controller2 = (controller_state >> 8) & 0xFF;
                }
            }    
        }
        else
            apu->cpu_writes(address, value);
    } 

    else if ((address >= 0x4020) && (address <= 0xFFFF))
        cart->cpu_writes(address, value);   
}



uint8_t Bus::ppu_reads(uint16_t address)
{
    uint8_t data = 0x00;
    
    if(address >= 0x0000 && address < 0x2000)
        data = cart->ppu_reads(address);

    return data;
}

void Bus::ppu_writes(uint16_t address, uint8_t value)
{
    cart->ppu_writes(address, value);
}

void Bus::set_nmi(bool value)
{
    cpu->set_nmi(value);
}

bool Bus::is_new_instruction()
{
    return cpu->is_new_instruction();
}

void Bus::soft_reset()
{
    NMI = false;
    shift_register_controller1 = shift_register_controller2 = 0x0000;
}

void Bus::set_zapper(bool zapper)
{
    zapper_connected = zapper;
    ppu->set_zapper(zapper);
}

void Bus::update_zapper_coordinates(int x, int y)
{
    zapper.x = x;
    zapper.y = y;

    zapper.trigger = 1;
    shift_register_controller2 &= 0xE6;
    shift_register_controller2 |= (zapper.trigger << 4);
}

void Bus::fire_zapper()
{
    zapper.trigger = 0;
    shift_register_controller2 = (shift_register_controller2 & ~(1 << 4)) | (zapper.trigger << 4);
    ppu->check_target_hit(zapper.x, zapper.y);
}

void Bus::set_light_sensed(bool hit)
{
    zapper.light_sensed = !hit;
    shift_register_controller2 = (shift_register_controller2 & ~(1 << 3)) | (zapper.light_sensed << 3);
}

void Bus::assert_irq(IRQ irq)
{
    IRQ_line |= irq;
}

void Bus::ack_irq(IRQ irq)
{
    IRQ_line = IRQ_line & (~irq);
}

uint8_t Bus::get_irq()
{
    return IRQ_line;
}

void Bus::set_irq_latch(uint8_t value)
{
    ppu->set_irq_latch(value);
}

void Bus::set_irq_enable(bool value)
{
    ppu->set_irq_enable(value);
}

void Bus::set_irq_reload()
{
    ppu->set_irq_reload();
}

void Bus::set_mapper(uint8_t value)
{
    ppu->set_mapper(value);
}

void Bus::set_mirroring_mode(MIRROR value)
{
    ppu->set_mirroring_mode(value);
}
