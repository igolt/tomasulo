//
// Created by joao-oliveira on 21/04/23.
//

#include "Tomasulo/utils.hpp"
#include "utils.hpp"

#include "Tomasulo.hpp"
#include "Tomasulo/instructions.hpp"

#include <string>

#define RISCV_EOI 0

// TODO(igolt): considerar offset ao tomar os registradores nas operações
#define RISCV_FP_REG_OFFSET 32

Tomasulo::Tomasulo(std::ostream& outstream):
    out(outstream)
{
    this->clear();
}

void Tomasulo::clear()
{
#define CLEAR(arr) memset(reinterpret_cast<void *>(arr), 0, sizeof(arr))
    CLEAR(this->instruction_memory);
    CLEAR(this->registers);
    CLEAR(this->register_status);
    CLEAR(this->_reservation_station);
}

void Tomasulo::load_instructions(std::istream& input_file)
{
    std::string  line;
    mips_word_t *instruction_memory_ptr = reinterpret_cast<mips_word_t *>(this->instruction_memory);

    this->clear();
    while (std::getline(input_file, line))
    {
        if (!line.empty() && line.front() != '#')
        {
            *instruction_memory_ptr++ = str_to_instruction(line);
        }
    }
    *instruction_memory_ptr = RISCV_EOI;
}

void Tomasulo::start(bool step_by_step)
{
    this->clock_cycle = 1;
    this->initialize_instruction_queue();
    do {
        this->execute();
        if (!this->instructions_queue.empty()) {
            this->issue();
        }
        this->write_result();

        this->out << "\nClock cycle: " << this->clock_cycle << '\n';
        this->memory_dump();
        this->register_dump();
        this->reservation_station_dump();
        if (step_by_step) {
            press_enter();
        }
        ++this->clock_cycle;
    } while (!this->instructions_queue.empty() || !this->reservation_station_is_empty());
}

void Tomasulo::initialize_instruction_queue()
{
    mips_word_t instruction;

    this->pc = 0;
    while ((instruction = _read_word(this->instruction_memory, this->pc)) != RISCV_EOI)
    {
        this->instructions_queue.push_back(instruction);
        this->pc += 4;
    }
}

void Tomasulo::execute()
{
    for (ReservationStation& rs : this->_reservation_station) {
        if (rs.busy && rs.qj == 0 && rs.qk == 0) {
            ++rs.cycles_executing;
        }
    }
}

void Tomasulo::issue()
{
    mips_word_t     instruction = this->instructions_queue.front();
    InstructionType i_type      = instruction_type(instruction);
    unsigned        r           = this->get_rs_num_for_instruction_type(i_type);

    if (r != 0)
    {
        mips_word_t rs, rt, rd;

        rs = instruction_rs1(instruction);
        rt = instruction_rs2(instruction);
        rd = instruction_rd(instruction) + RISCV_FP_REG_OFFSET;
        switch (i_type)
        {
            case InstructionType::ADD:
            case InstructionType::SUB:
            case InstructionType::MULT:
            case InstructionType::DIV:
                rs += RISCV_FP_REG_OFFSET;
                rt += RISCV_FP_REG_OFFSET;

                if (this->register_status[rs])
                {
                    this->reservation_station(r).qj = this->register_status[rs];
                }
                else
                {
                    this->reservation_station(r).vj = this->registers[rs];
                    this->reservation_station(r).qj = 0;
                }

                if (this->register_status[rt])
                {
                    this->reservation_station(r).qk = this->register_status[rt];
                }
                else
                {
                    this->reservation_station(r).vk = this->registers[rt];
                    this->reservation_station(r).qk = 0;
                }
                this->register_status[rd] = r;
                break;
            case InstructionType::LW:
            case InstructionType::SW:

                if (this->register_status[rs])
                {
                    this->reservation_station(r).qj = this->register_status[rs];
                }
                else
                {
                    this->reservation_station(r).vj = this->registers[rs];
                    this->reservation_station(r).qj = 0;
                }
                // TODO(igolt): puta que pariu immediate é de fuder
                // this->reservation_station(r).a = immediate

                if (i_type == InstructionType::LW)
                {
                    this->register_status[rt] = r;
                }
                else
                {
                    if (this->register_status[rt])
                    {
                        this->reservation_station(r).qk = this->register_status[rt];
                    }
                    else
                    {
                        this->reservation_station(r).vk = this->registers[rt];
                        this->reservation_station(r).qk = 0;
                    }
                }
                break;
        }
        this->reservation_station(r).busy             = true;
        this->reservation_station(r).operation        = i_type;
        this->reservation_station(r).cycles_executing = 0;
        this->instructions_queue.pop_front();
    }
}

void Tomasulo::write_result()
{
    unsigned rs_num;
    for (rs_num = 1; rs_num <= ARRAYSIZE(this->_reservation_station); ++rs_num) {
        ReservationStation& rs = this->reservation_station(rs_num);

        if (rs.busy && rs.cycles_executing >= instruction_execution_cycles(rs.operation)) {
            mips_word_t result = this->get_reservation_station_result(rs);
            rs.busy = false;

            for (ReservationStation& rs2 : this->_reservation_station) {
                if (rs2.busy) {
                    if (rs2.qk == rs_num) {
                        rs2.qk = 0;
                        rs2.vk = result;
                    } else if (rs2.qj == rs_num) {
                        rs2.qj = 0;
                        rs2.vj = result;
                    }
                }
            }
            for (unsigned i = 0; i < ARRAYSIZE(this->register_status); ++i) {
                if (this->register_status[i] == rs_num) {
                    this->registers[i] = result;
                    this->register_status[i] = 0;
                }
            }
        }
    }
}

mips_word_t Tomasulo::get_reservation_station_result(const ReservationStation& rs)
{
    switch (rs.operation) {
        case InstructionType::ADD:
            return rs.vj + rs.vk;
        case InstructionType::SUB:
            return rs.vj - rs.vk;
        case InstructionType::MULT:
            return rs.vj * rs.vk;
        case InstructionType::DIV:
            return rs.vj / rs.vk;
        case InstructionType::LW:
            return _read_word(this->data_memory, rs.a);
        case InstructionType::SW:
            return 0;
    }
    throw std::invalid_argument(__FUNCTION__);
}

unsigned Tomasulo::get_rs_num_for_instruction_type(InstructionType i_type)
{
    unsigned lower, upper;

    switch (i_type)
    {
        case InstructionType::LW:
        case InstructionType::SW:
            lower = 0, upper = 2;
            break;
        case InstructionType::ADD:
        case InstructionType::SUB:
            lower = 2, upper = 5;
            break;
        case InstructionType::DIV:
        case InstructionType::MULT:
            lower = 5, upper = 7;
            break;
    }

    for (unsigned i = lower; i < upper; ++i)
    {
        if (!this->_reservation_station[i].busy)
            return i + 1;
    }
    return 0;
}

ReservationStation& Tomasulo::reservation_station(unsigned r)
{
    return this->_reservation_station[r - 1];
}

bool Tomasulo::reservation_station_is_empty()
{
    for (const ReservationStation& rs : this->_reservation_station) {
        if (rs.busy) {
            return false;
        }
    }
    return true;
}

void Tomasulo::write_fp_to_memory(mips_float_t f, mips_word_t address)
{
    *reinterpret_cast<mips_word_t *>(this->data_memory[address]) = *reinterpret_cast<mips_word_t *>(&f);
}

mips_float_t Tomasulo::read_fp_from_memory(mips_word_t address)
{
    return *reinterpret_cast<mips_float_t *>(this->data_memory[address]);
}
