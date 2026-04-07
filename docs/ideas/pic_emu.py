#!/usr/bin/env python3
"""
PIC I2C Protocol Emulator — Unicorn Engine
Runs stock kernel 3.10.14 PIC functions and traces SM0 register accesses.
Emulates PIC16LF1509 I2C slave responses.
"""

from unicorn import *
from unicorn.mips_const import *
import struct
import sys

# === Memory layout ===
KERNEL_BASE = 0x80001000
KERNEL_FILE = "/Users/igor/Yandex.Disk.localized/projects/almond/full_ram.bin"

# Palmbus KSEG1 (uncached) — SM0 I2C controller
PALMBUS_PHYS = 0x1E000000
PALMBUS_KSEG1 = 0xBE000000
PALMBUS_SIZE = 0x10000

# Stack (after kernel)
STACK_ADDR = 0x82000000
STACK_SIZE = 0x100000

# Data area (for PIC state machine variables)
DATA_ADDR = 0x82200000
DATA_SIZE = 0x100000

# BSS / heap
HEAP_ADDR = 0x82400000
HEAP_SIZE = 0x400000

# === SM0 Register offsets ===
SM0_REGS = {
    0x034: "RSTCTRL",
    0x900: "SM0_CFG",
    0x908: "SM0_DATA/CTL0",
    0x90C: "SM0_SLAVE",
    0x910: "SM0_DATAOUT",
    0x914: "SM0_DATAIN",
    0x918: "SM0_POLLSTA",
    0x91C: "SM0_STATUS/CTL1",
    0x920: "SM0_START",
    0x928: "SM0_CFG2",
    0x940: "N_CTL0/SM0_CTL1",
    0x944: "N_CTL1",
    0x950: "N_D0",
    0x954: "N_D1",
}

# === PIC Emulation State ===
class PICState:
    def __init__(self):
        self.write_buf = []
        self.read_buf = []
        self.read_idx = 0
        self.adc_value = 0x0237  # ~567 = normal battery
        self.mode = 0  # 0=idle, 1=bat_read_pending
        self.ack = True

    def on_write(self, data):
        """PIC receives I2C write data"""
        self.write_buf.extend(data)
        cmd = self.write_buf[0] if self.write_buf else 0
        print(f"  [PIC] WRITE received: {[f'0x{b:02X}' for b in self.write_buf]}")

        if cmd == 0x33:  # WAKE
            count = (self.write_buf[1] << 8 | self.write_buf[2]) if len(self.write_buf) >= 3 else 0
            print(f"  [PIC] WAKE count={count}")
        elif cmd == 0x2D:
            print(f"  [PIC] Table1 ({len(self.write_buf)-1} data bytes)")
        elif cmd == 0x2E:
            print(f"  [PIC] Table2 ({len(self.write_buf)-1} data bytes)")
        elif cmd == 0x2F:
            mode = self.write_buf[2] if len(self.write_buf) >= 3 else 0
            print(f"  [PIC] bat_read mode={mode} ({'polling' if mode == 1 else 'oneshot'})")
            self.mode = 1
            # Prepare read response: ADC high, ADC low, status bytes
            hi = (self.adc_value >> 8) & 0xFF
            lo = self.adc_value & 0xFF
            self.read_buf = [hi, lo, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00]
            self.read_idx = 0
            print(f"  [PIC] Prepared read data: {[f'0x{b:02X}' for b in self.read_buf]}")
        elif cmd == 0x34:
            buz_mode = self.write_buf[2] if len(self.write_buf) >= 3 else 0
            print(f"  [PIC] Buzzer mode={buz_mode}")

        self.write_buf = []

    def on_read(self):
        """PIC sends I2C read data"""
        if self.read_idx < len(self.read_buf):
            val = self.read_buf[self.read_idx]
            self.read_idx += 1
            print(f"  [PIC] READ byte {self.read_idx-1}: 0x{val:02X}")
            return val
        print(f"  [PIC] READ: no more data, returning 0xFF")
        return 0xFF

pic = PICState()

# === SM0 Register State ===
sm0_regs = {}
write_count = 0
write_data = []
read_mode = False
read_count = 0

def sm0_name(off):
    return SM0_REGS.get(off, f"UNK_{off:03X}")

def hook_mem_write(uc, access, address, size, value, user_data):
    global write_count, write_data, read_mode, read_count

    if PALMBUS_KSEG1 <= address < PALMBUS_KSEG1 + PALMBUS_SIZE:
        off = address - PALMBUS_KSEG1
        name = sm0_name(off)

        # Only log SM0-related registers
        if off >= 0x034 and (off <= 0x034 or (off >= 0x900 and off <= 0x960)):
            print(f"  SM0 WRITE {name} (0x{off:03X}) = 0x{value:08X}")
            sm0_regs[off] = value

            # Track I2C protocol
            if off == 0x908:  # SM0_DATA = slave address
                addr7 = value & 0x7F
                rw = "READ" if value & 1 else "WRITE"
                print(f"    → Slave addr=0x{addr7:02X} ({rw})")

            elif off == 0x910:  # SM0_DATAOUT
                write_data.append(value & 0xFF)
                print(f"    → Data byte: 0x{value & 0xFF:02X}")

            elif off == 0x91C:  # SM0_STATUS/CTL1 — mode
                modes = {0: "WRITE", 1: "READ", 2: "WRITE+READ"}
                m = modes.get(value, f"UNKNOWN({value})")
                print(f"    → Mode: {m}")
                if value == 1:
                    read_mode = True
                elif value == 0:
                    read_mode = False

            elif off == 0x920:  # SM0_START
                if read_mode:
                    read_count = value + 1  # PIC_I2C_READ uses count-1
                    print(f"    → READ start, count={read_count}")
                else:
                    write_count = value
                    print(f"    → WRITE/START count={value}")
                    if value == 0 and write_data:
                        pic.on_write(write_data)
                        write_data = []

            elif off == 0x900:  # SM0_CFG
                print(f"    → CFG = 0x{value:02X} *** KEY CONFIG ***")

            elif off == 0x940:  # N_CTL0/SM0_CTL1
                print(f"    → CTL1/N_CTL0 = 0x{value:08X}")

def hook_mem_read(uc, access, address, size, value, user_data):
    if PALMBUS_KSEG1 <= address < PALMBUS_KSEG1 + PALMBUS_SIZE:
        off = address - PALMBUS_KSEG1

        if off >= 0x900 and off <= 0x960:
            name = sm0_name(off)

            if off == 0x914:  # SM0_DATAIN — return PIC data
                val = pic.on_read()
                uc.mem_write(address, struct.pack("<I", val))
                print(f"  SM0 READ  {name} (0x{off:03X}) → 0x{val:02X}")
                return

            elif off == 0x918:  # SM0_POLLSTA — always ready
                val = 0x07  # bit0=complete, bit1=write_ready, bit2=read_ready
                uc.mem_write(address, struct.pack("<I", val))
                return  # Don't spam log

            elif off == 0x900:  # SM0_CFG
                val = sm0_regs.get(0x900, 0)
                # On our hardware this is READ-ONLY=0, but stock kernel may expect different
                print(f"  SM0 READ  {name} (0x{off:03X}) → 0x{val:08X}")
                return

            else:
                val = sm0_regs.get(off, 0)
                print(f"  SM0 READ  {name} (0x{off:03X}) → 0x{val:08X}")

# === Stub functions ===
STUB_RET = 0x80002000  # Address for stub return

def hook_code(uc, address, size, user_data):
    """Intercept kernel function calls we can't emulate"""
    vaddr = address

    # udelay / mdelay — just skip
    if vaddr == 0x80249210:  # udelay
        a0 = uc.reg_read(UC_MIPS_REG_A0)
        # Silent — too many calls
        uc.reg_write(UC_MIPS_REG_PC, uc.reg_read(UC_MIPS_REG_RA))
        return

    # mutex lock/unlock — skip
    if vaddr in (0x8052A614, 0x8052A264):
        uc.reg_write(UC_MIPS_REG_PC, uc.reg_read(UC_MIPS_REG_RA))
        return

    # printk — skip
    if vaddr == 0x80043B88:
        uc.reg_write(UC_MIPS_REG_PC, uc.reg_read(UC_MIPS_REG_RA))
        return

def hook_intr(uc, intno, user_data):
    """Handle syscalls/exceptions"""
    pc = uc.reg_read(UC_MIPS_REG_PC)
    print(f"  [EXCEPTION] intno={intno} PC=0x{pc:08X}")
    uc.emu_stop()

# === Main ===
def main():
    print("=== PIC I2C Protocol Emulator ===")
    print(f"Loading kernel from {KERNEL_FILE}...")

    with open(KERNEL_FILE, "rb") as f:
        kernel_data = f.read()
    print(f"Kernel size: {len(kernel_data)} bytes")

    # Create MIPS32 LE emulator
    mu = Uc(UC_ARCH_MIPS, UC_MODE_MIPS32 | UC_MODE_LITTLE_ENDIAN)

    # Map memory regions
    # Kernel text+data+bss+heap — map a big chunk covering everything
    # Stock kernel uses: text 0x80001000, data ~0x80700000, bss ~0x81300000
    KERNEL_MAP_START = 0x80000000
    KERNEL_MAP_SIZE = 0x2000000  # 32MB — covers kernel + data + bss
    mu.mem_map(KERNEL_MAP_START, KERNEL_MAP_SIZE, UC_PROT_ALL)
    mu.mem_write(KERNEL_BASE, kernel_data)
    print(f"Kernel mapped at 0x{KERNEL_MAP_START:08X}, size={KERNEL_MAP_SIZE:#x}")

    # Palmbus (KSEG1: 0xBE000000)
    mu.mem_map(PALMBUS_KSEG1, PALMBUS_SIZE, UC_PROT_ALL)
    # Init SM0 registers to stock defaults
    mu.mem_write(PALMBUS_KSEG1 + 0x940, struct.pack("<I", 0x90640042))  # SM0_CTL1

    # Stack (above kernel region)
    mu.mem_map(STACK_ADDR, STACK_SIZE, UC_PROT_ALL)

    # Stub return area (use addr in kernel map)
    global STUB_RET
    STUB_RET = 0x81FFE000
    # Write a "jr $ra; nop" at stub
    mu.mem_write(STUB_RET, b'\x08\x00\xe0\x03\x00\x00\x00\x00')  # jr ra; nop

    # Add hooks
    mu.hook_add(UC_HOOK_MEM_WRITE, hook_mem_write)
    mu.hook_add(UC_HOOK_MEM_READ, hook_mem_read)
    mu.hook_add(UC_HOOK_INTR, hook_intr)

    # Setup registers
    mu.reg_write(UC_MIPS_REG_SP, STACK_ADDR + STACK_SIZE - 0x100)
    mu.reg_write(UC_MIPS_REG_GP, 0x80700000)  # typical GP
    mu.reg_write(UC_MIPS_REG_RA, STUB_RET)  # return to stub

    # === Test 1: PIC_I2C_WRITE ===
    print("\n" + "="*60)
    print("TEST 1: PIC_I2C_WRITE — write {0x2F, 0x00, 0x01}")
    print("="*60)

    # PIC_I2C_WRITE @ 0x80412F78
    # void PIC_I2C_WRITE(uint8_t cmd, int data_ptr, int count)
    # a0 = cmd (first byte), a1 = data_ptr, a2 = count

    # Write bat_read command data to memory
    bat_cmd = bytes([0x2F, 0x00, 0x01])
    cmd_addr = 0x81F00000  # temp buffer in kernel map
    mu.mem_write(cmd_addr, bat_cmd)

    mu.reg_write(UC_MIPS_REG_A0, 0x2F)  # cmd byte
    mu.reg_write(UC_MIPS_REG_A1, cmd_addr + 1)  # rest of data
    mu.reg_write(UC_MIPS_REG_A2, 3)  # count
    mu.reg_write(UC_MIPS_REG_RA, STUB_RET)
    mu.reg_write(UC_MIPS_REG_SP, STACK_ADDR + STACK_SIZE - 0x100)

    try:
        mu.emu_start(0x80412F78, STUB_RET, timeout=5000000, count=10000)
        print("[WRITE done]")
    except UcError as e:
        pc = mu.reg_read(UC_MIPS_REG_PC)
        print(f"[WRITE stopped] {e} at PC=0x{pc:08X}")

    # === Test 2: PIC_I2C_READ ===
    print("\n" + "="*60)
    print("TEST 2: PIC_I2C_READ — read 8 bytes")
    print("="*60)

    # PIC_I2C_READ @ 0x80412E78
    # void PIC_I2C_READ(int buf_ptr, int count)
    read_buf_addr = 0x81F01000  # temp buffer in kernel map
    mu.mem_write(read_buf_addr, b'\x00' * 16)

    mu.reg_write(UC_MIPS_REG_A0, read_buf_addr)
    mu.reg_write(UC_MIPS_REG_A1, 8)  # read 8 bytes
    mu.reg_write(UC_MIPS_REG_RA, STUB_RET)
    mu.reg_write(UC_MIPS_REG_SP, STACK_ADDR + STACK_SIZE - 0x100)

    # Reset PIC read buffer
    pic.read_idx = 0

    try:
        mu.emu_start(0x80412E78, STUB_RET, timeout=5000000, count=50000)
        print("[READ done]")
    except UcError as e:
        pc = mu.reg_read(UC_MIPS_REG_PC)
        print(f"[READ stopped] {e} at PC=0x{pc:08X}")

    # Read result
    result = mu.mem_read(read_buf_addr, 8)
    print(f"\nRead buffer: {[f'0x{b:02X}' for b in result]}")

    print("\n=== SM0 Register Summary ===")
    for off in sorted(sm0_regs.keys()):
        print(f"  {sm0_name(off):20s} (0x{off:03X}) = 0x{sm0_regs[off]:08X}")

if __name__ == "__main__":
    main()
