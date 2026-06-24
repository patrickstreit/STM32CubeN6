# Notes
Guide Followed:
https://community.st.com/stm32-mcus-60/how-to-debug-the-stm32n6-using-vs-code-156102

3. Create an STM32N6 CMake project (done)
4. Code editing and post build commands (done)
5. Build the project (done)
6. Create and modify the launch.json file (done)
    - see firmware.code-workspace in SensorPlatform
7. Create and modify tasks.json (done)
    - see firmware.code-workspace in SensorPlatform
8. Start debugging (ongoing)
    - Debug starts
    - Breakpoint in FSBL reached --> BOOT_Application
    - JumpToApp fails silently

Debug Logdump:

Cortex-Debug: VSCode debugger extension version 1.12.1 git(652d042). Usage info: https://github.com/Marus/cortex-debug#usage
Reading symbols from C:/Users/cross-INGPatrickStre/AppData/Local/stm32cube/bundles/gnu-tools-for-stm32/14.3.1+st.2/bin/arm-none-eabi-objdump.exe --syms -C -h -w C:/repositories/STM32CubeN6/Projects/STM32N6570-DK/Examples/Blinky_XiP/FSBL/build/Blinky_XiP_FSBL.elf
Reading symbols from c:/users/cross-ingpatrickstre/appdata/local/stm32cube/bundles/gnu-tools-for-stm32/14.3.1+st.2/bin/arm-none-eabi-nm.exe --defined-only -S -l -C -p C:/repositories/STM32CubeN6/Projects/STM32N6570-DK/Examples/Blinky_XiP/FSBL/build/Blinky_XiP_FSBL.elf
Launching GDB: "C:\\Users\\cross-INGPatrickStre\\AppData\\Local\\stm32cube\\bundles\\gnu-tools-for-stm32\\14.3.1+st.2\\bin\\arm-none-eabi-gdb.exe" -q --interpreter=mi2
    IMPORTANT: Set "showDevDebugOutput": "raw" in "launch.json" to see verbose GDB transactions here. Very helpful to debug issues or report problems
Setting GDB-Server CWD: C:\Users\cross-INGPatrickStre\AppData\Local\stm32cube\bundles/stlink-gdbserver/7.13.0+st.3/bin
Launching gdb-server: "C:\\Users\\cross-INGPatrickStre\\AppData\\Local\\stm32cube\\bundles/stlink-gdbserver/7.13.0+st.3/bin/ST-LINK_gdbserver.exe" -p 50000 -cp "C:\\Users\\cross-INGPatrickStre\\AppData\\Local\\stm32cube\\bundles/programmer/2.22.0+st.1/bin" --swd --halt -m 1 -el "C:/Program Files/ST/STM32CubeProgrammer_2.20.0/bin/ExternalLoader/MX66UW1G45G_STM32N6570-DK.stldr"
    Please check TERMINAL tab (gdb-server) for output from C:\Users\cross-INGPatrickStre\AppData\Local\stm32cube\bundles/stlink-gdbserver/7.13.0+st.3/bin/ST-LINK_gdbserver.exe
Finished reading symbols from objdump: Time: 1069 ms
Finished reading symbols from nm: Time: 954 ms
Output radix now set to decimal 10, hex a, octal 12.
Input radix now set to decimal 10, hex a, octal 12.
0x18003a1a in ?? ()
Program stopped, probably due to a reset and/or halt issued by debugger
Trying to halt core...
add symbol table from file "C:/repositories/STM32CubeN6/Projects/STM32N6570-DK/Examples/Blinky_XiP/Appli/build/Blinky_XiP_Appli.elf"
(y or n) [answered Y; input not from terminal]
Reading symbols from C:/repositories/STM32CubeN6/Projects/STM32N6570-DK/Examples/Blinky_XiP/Appli/build/Blinky_XiP_Appli.elf...
Failed to update peripheral XSPI2: Error: peripheral-viewer: readMemory failed @ 0x4802a000 for 40 bytes: CodeExpectedError: Busy, session=cb20d502-2be5-4fb9-b28d-799e6e4d7bd3

Temporary breakpoint 2, BOOT_Application () at C:/repositories/STM32CubeN6/Projects/STM32N6570-DK/Examples/Blinky_XiP/Middlewares/ST/STM32_ExtMem_Manager/boot/stm32_boot_xip.c:65
65	  retr = MapMemory();
Attempt to use a type name as an expression.

Breakpoint 1, JumpToApplication () at C:/repositories/STM32CubeN6/Projects/STM32N6570-DK/Examples/Blinky_XiP/Middlewares/ST/STM32_ExtMem_Manager/boot/stm32_boot_xip.c:127
127	  if (EXTMEM_OK != EXTMEM_GetMapAddress(EXTMEM_MEMORY_BOOTXIP, &Application_vector))

Program
 received signal SIGTRAP, Trace/breakpoint trap.
EXTMEM_GetMapAddress (MemId=0, BaseAddress=0x341fffc0) at C:/repositories/STM32CubeN6/Projects/STM32N6570-DK/Examples/Blinky_XiP/Middlewares/ST/STM32_ExtMem_Manager/stm32_extmem.c:1182
1182	}
Output radix now set to decimal 16, hex 10, octal 20.
Input radix now set to decimal 10, hex a, octal 12.
warning: Remote failure reply: E31

Program
 stopped.
Cannot remove breakpoints because program is no longer writable.
Further execution is probably impossible.
JumpToApplication () at C:/repositories/STM32CubeN6/Projects/STM32N6570-DK/Examples/Blinky_XiP/Middlewares/ST/STM32_ExtMem_Manager/boot/stm32_boot_xip.c:159
159	  JumpToApp = (pFunction)(*(__IO uint32_t *)(Application_vector + 4u));
warning: error removing breakpoint 1 at 0x3418c08a
Watchpoint 3 deleted because the program has left the block
in which its expression is valid.
2
warning: error removing breakpoint -1 at 0x3418c00c
Warning:
Cannot insert breakpoint 4.
Cannot access memory at address 0x3418c08a
Cannot insert breakpoint 5.
Cannot access memory at address 0x3418c192

GDB Error? continue command is reported as error after initially succeeding. token '261'
Error: A serious error occurred with gdb, unable to continue or interrupt We may not be able to recover from this point. You can try continuing or ending session. Must address root cause though
Command aborted.
Failed to update peripheral XSPI2: Error: peripheral-viewer: readMemory failed @ 0x4802a000 for 40 bytes: CodeExpectedError: Read memory error: Unable to read memory. (from data-read-memory-bytes "0x4802a000" 40), session=cb20d502-2be5-4fb9-b28d-799e6e4d7bd3
Failed to update peripheral XSPI2: Error: peripheral-viewer: readMemory failed @ 0x4802a040 for 20 bytes: CodeExpectedError: Read memory error: Unable to read memory. (from data-read-memory-bytes "0x4802a040" 20), session=cb20d502-2be5-4fb9-b28d-799e6e4d7bd3
Failed to update peripheral XSPI2: Error: peripheral-viewer: readMemory failed @ 0x4802a080 for 20 bytes: CodeExpectedError: Read memory error: Unable to read memory. (from data-read-memory-bytes "0x4802a080" 20), session=cb20d502-2be5-4fb9-b28d-799e6e4d7bd3
Failed to update peripheral XSPI2: Error: peripheral-viewer: readMemory failed @ 0x4802a100 for 100 bytes: CodeExpectedError: Read memory error: Unable to read memory. (from data-read-memory-bytes "0x4802a100" 100), session=cb20d502-2be5-4fb9-b28d-799e6e4d7bd3
Failed to update peripheral XSPI2: Error: peripheral-viewer: readMemory failed @ 0x4802a180 for 36 bytes: CodeExpectedError: Read memory error: Unable to read memory. (from data-read-memory-bytes "0x4802a180" 36), session=cb20d502-2be5-4fb9-b28d-799e6e4d7bd3
Failed to update peripheral XSPI2: Error: peripheral-viewer: readMemory failed @ 0x4802a200 for 44 bytes: CodeExpectedError: Read memory error: Unable to read memory. (from data-read-memory-bytes "0x4802a200" 44), session=cb20d502-2be5-4fb9-b28d-799e6e4d7bd3
GDB session ended. exit-code: 0