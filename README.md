# ToxenOS

ToxenOS is a small, experimental 32-bit hobby operating system built completely from scratch.  
It boots using GRUB, switches into protected mode, prints a custom welcome screen at the top,  
and provides a minimal text console with keyboard input and a visible hardware cursor.

This project is designed for learning OS development step-by-step, keeping the kernel simple,  
clean, and easy to extend.

---

## 🖥️ Current Boot Screen

            welcome to ToxenOS
                   v0.0.1

---

## ✨ Features (Stage 1)

- Custom 32-bit kernel written in C + assembly  
- GRUB multiboot-compliant entry  
- Clean VGA text-mode console  
- Welcome message at the top of the screen  
- Visible hardware text cursor  
- Keyboard input using polling  
- Backspace support  
- Automatic scrolling when reaching bottom  
- Basic command echoing  
- MIT-licensed open-source project  

---

## 🔧 Build Instructions

### Requirements
Install:

--------for ubuntu
gcc
binutils
make
grub-pc-bin (or grub2 tools)
xorriso
qemu-system-x86
--------


### Build the kernel + ISO
Run:


### Boot in QEMU
Run:


---

## 📁 Project Structure

ToxenOS/
├── boot/
│ └── grub/
│ └── grub.cfg
├── src/
│ ├── boot.s # Multiboot entry + stack setup
│ ├── kernel.c # Core kernel (console, VGA, input)
│ └── linker.ld # Linker script for kernel layout
├── Makefile # Build system for kernel + ISO
└── LICENSE # MIT License


---

## 🛣️ Roadmap

### Stage 2 — Console Improvements
- Command parser (`help`, `clear`, `echo`, `reboot`)  
- Better text editing  
- Colored UI  
- Optional ASCII art splash logo  

### Stage 3 — Hardware & Kernel Features
- IDT + interrupt handling  
- PIC remapping  
- IRQ-based keyboard  
- PIT timer interrupts  
- Uptime counter / blinking cursor  
- Memory map detection  
- Basic physical memory allocator  

### Stage 4 — Advanced OS Features
- Paging / virtual memory  
- System calls  
- Basic filesystem support  
- User-space experiments  

---

## 🤝 Contributing

ToxenOS is open-source and MIT-licensed.  
Contributions, experiments, forks, and learning projects are all welcome.

---

## 📜 License

This project is licensed under the MIT License.  
See the `LICENSE` file for details.
