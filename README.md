# Stellux Motivation
Stellux OS is an operating system project inspired from my research with Tommy Unger and Jonathan Appavoo at Boston University. The work that inspired it is Symbiote - a chronokernel approach to dissolving the barrier between userspace and kernelspace. The goal of Symbiote is to provide an "elevation" mechanism for userspace threads to allow them to run at privileged supervisor level. The benefit of this approach is that it combines debuggability and ease of development of userspace applications, encourages the use of third-party libraries, while providing functionality and performance benefits of a kernel module.<br/>
The goal for Stellux operating system is to be developed stemming and being built on top of Symbiote's philosophy that userspace threads can make a runtime choice of elevating themselves to kernel level. This unlocks a whole new world where there are no kernel-specific modules or drivers, but everything is a userspace application. Drivers and kernel modules can be normal userspace applications that elevate themselves at will whenever they need to either take critical paths or make use of kernel resources.<br/>
Additionally, Stellux is a learning project for myself to learn and practice OS development on a more unique and in-depth level.

## Supported Architectures

| x86 | ARM64 |
|:--------:| :-:
| ✓    | X

## Building
TO-DO: describe the building process

## Dependencies
TO-DO: describe devel-tools installation process

## Contributing
Pull requests are welcome. For major changes, please open an issue first to discuss what you would like to change.

## License
[MIT](https://opensource.org/license/mit/)
