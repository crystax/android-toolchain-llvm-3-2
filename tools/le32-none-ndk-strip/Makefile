LEVEL := ../..

TOOLNAME = le32-none-ndk-strip

LINK_COMPONENTS := asmparser bitreader bitwriter codegen \
                   instrumentation ipo linker selectiondag

USEDLIBS := LLVMWrap.a

# This tool has no plugins, optimize startup time.
TOOL_NO_EXPORTS := 1

include $(LEVEL)/Makefile.common

