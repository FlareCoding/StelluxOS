<div align="center">

  <img src="screenshots/usb_stick_logo.jpg" alt="logo" width="200" height="auto" />
  <h1>Stellux OS</h1>
  
  <p>
    StelluxOS is a personal operating system project inspired by Symbiote project's philosophy of providing runtime privilege
    level switching mechanism for userspace threads. This would ultimately allow the kernel to be a pool of hot-swappable and
    restartable components that can elevate themselves when performing privileged tasks and staying lowered the rest of the time.
  </p>
  
  <!-- Badges -->
  <p>
    <a href="https://github.com/FlareCoding/StelluxOS/actions/workflows/ci.yml">
      <img src="https://github.com/FlareCoding/StelluxOS/actions/workflows/ci.yml/badge.svg?branch=master" alt="badge" />
    </a>
    <a href="https://github.com/FlareCoding/StelluxOS/graphs/contributors">
      <img src="https://img.shields.io/github/contributors/FlareCoding/StelluxOS" alt="contributors" />
    </a>
    <a href="">
      <img src="https://img.shields.io/github/last-commit/FlareCoding/StelluxOS" alt="last update" />
    </a>
    <a href="https://github.com/FlareCoding/StelluxOS/network/members">
      <img src="https://img.shields.io/github/forks/FlareCoding/StelluxOS" alt="forks" />
    </a>
    <a href="https://github.com/FlareCoding/StelluxOS/stargazers">
      <img src="https://img.shields.io/github/stars/FlareCoding/StelluxOS" alt="stars" />
    </a>
    <a href="https://github.com/FlareCoding/StelluxOS/issues/">
      <img src="https://img.shields.io/github/issues/FlareCoding/StelluxOS" alt="open issues" />
    </a>
    <a href="https://github.com/FlareCoding/StelluxOS/blob/master/LICENSE">
      <img src="https://img.shields.io/github/license/FlareCoding/StelluxOS.svg" alt="license" />
    </a>
  </p>
   
  <h4>
    <a href="https://github.com/FlareCoding/StelluxOS/">View Demo</a>
    <span> · </span>
    <a href="https://github.com/FlareCoding/StelluxOS">Documentation</a>
    <span> · </span>
    <a href="https://github.com/FlareCoding/StelluxOS/issues/">Report Bug</a>
    <span> · </span>
    <a href="https://github.com/FlareCoding/StelluxOS/issues/">Request Feature</a>
  </h4>
</div>

<br />

<!-- Table of Contents -->
# :book: Table of Contents

- [About the Project](#star2-about-the-project)
  * [Screenshots](#camera-screenshots)
  * [Supported Architectures](#desktop_computer-supported-architectures)
  * [Features](#dart-features)
- [Getting Started](#gear-getting-started)
  * [Prerequisites](#bangbang-prerequisites)
  * [Building and Running](#hammer_and_wrench-building-and-running-the-project)
  * [Debugging](#wrench-debugging)
- [Roadmap](#compass-roadmap)
- [Contributing](#wave-contributing)
- [License](#newspaper-license)
- [Acknowledgements](#gem-acknowledgements)

  

<!-- About the Project -->
## :star2: About the Project
Stellux OS is an operating system project inspired from my research with Tommy Unger and Jonathan Appavoo at Boston University. The work that inspired it is
Symbiote - a chronokernel approach to dissolving the barrier between userspace and kernelspace. The motivation behind Symbiote was to provide an "elevation" mechanism for
userspace threads to allow them to run at privileged supervisor level. The benefit of this approach is that it combines debuggability and ease of development of
userspace applications, encourages the use of third-party libraries, while providing functionality and performance benefits of a kernel module.

The goal for Stellux operating system is to be developed stemming and being built on top of Symbiote's philosophy that userspace threads can make a runtime choice
of elevating themselves to kernel level. This design allows the kernel to be a pool of userspace components that can be monitored by a _watchdog_ process and if any
part of the kernel fails, rather than crashing the whole system, that individual component could be restarted or hot-swapped.
This unlocks a whole new world where there are no kernel-specific modules or drivers, but everything is a userspace application. Drivers and kernel modules can be normal
userspace applications that elevate themselves at will whenever they need to either take critical paths or make use of kernel resources.

<!-- Screenshots -->
### :camera: Screenshots

<div align="center"> 
  <img src="screenshots/stellux-run.png" alt="screenshot" />
  <br/>
  <img src="screenshots/stellux-xhci-run.png" alt="screenshot" />
</div>


<!-- TechStack -->
### :desktop_computer: Supported Architectures

| x86 | ARM64 |
|:--------:| :-:
| ✓    | X

<!-- Features -->
### :dart: Features

- Userspace and syscall support
- Multithreading
- SMP multicore support
- Kernel and userspace thread management
- _kElevate_/_kLower_ mechanisms for runtime privilege switching
- PCI device enumeration
- Optimized write-combining graphics buffer management
- HPET and time measuring support
- Stacktrace dump from the _interrupt_ context

<!-- Getting Started -->
## :gear: Getting Started

<!-- Prerequisites -->
### :bangbang: Prerequisites

Clone the repository
```bash
git clone https://github.com/FlareCoding/StelluxOS.git
```

Install dependencies
```bash
make install-dependencies
```

<!-- Building and Running the Project -->
### :hammer_and_wrench: Building and Running the Project

To build the __Stellux__ image, simply run
```bash
make
```

Running in QEMU in a separate graphical window
```bash
make run
```

Running in QEMU headless in the current shell<br/>
*Note: use this if in _ssh_ session*
```bash
make run-headless
```

<!-- Debuggin -->
### :wrench: Debugging

1) Run a headless QEMU session in the current shell
```bash
make run-debug-headless
```
*Note: This will hang until a GDB client connects to the stub*

2) In a separate shell connect to the GDB server
```bash
make connect-gdb
```
*Note: Type 'y' in the prompt and continue, the kernel will hit a breakpoint on kernel entry*


<!-- Roadmap -->
## :compass: Roadmap

* [x] Write-combining graphics framebuffer optimization
* [x] Arbitrary kLower mechanism
* [x] Multithreading
* [x] AP processor startup (leads into SMP)
* [x] SMP support in the scheduler
* [x] Unit testing infrastructure
* [x] Improved thread creation & management mechanism
* [x] xhci controller driver
* [x] USB support for a generic HID keyboard and mouse
* [ ] Complete rework of the kprint and dmesg interface
* [ ] GUI and window management subsystem
* [ ] Virtual filesystem and initramfs
* [ ] Kernel command-line argument support


<!-- Contributing -->
## :wave: Contributing

<a href="https://github.com/FlareCoding/StelluxOS/graphs/contributors">
  <img src="https://contrib.rocks/image?repo=FlareCoding/StelluxOS" />
</a>


Contributions are always welcome through pull requests.

<!-- License -->
## :newspaper: License

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)


<!-- Acknowledgments -->
## :gem: Acknowledgements

Special thanks to Dr. Tommy Unger for the Symbiote and kElevate work and Dr. Jonathan Appavoo for supporting
this project from inception and all the technical knowledge they brought to the table. 

