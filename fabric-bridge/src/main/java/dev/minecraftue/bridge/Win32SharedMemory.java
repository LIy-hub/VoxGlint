package dev.minecraftue.bridge;

import java.lang.foreign.Arena;
import java.lang.foreign.FunctionDescriptor;
import java.lang.foreign.Linker;
import java.lang.foreign.MemorySegment;
import java.lang.foreign.SymbolLookup;
import java.lang.foreign.ValueLayout;
import java.lang.invoke.MethodHandle;
import java.nio.charset.StandardCharsets;

/** Pure Java 25 FFM wrapper for Windows named shared memory (no JNI DLL or JNA overhead). */
final class Win32SharedMemory implements AutoCloseable {
    private static final int PAGE_READWRITE = 0x04;
    private static final int FILE_MAP_ALL_ACCESS = 0x000F001F;
    private static final MemorySegment INVALID_HANDLE_VALUE = MemorySegment.ofAddress(-1L);

    private static final Linker LINKER = Linker.nativeLinker();
    private static final SymbolLookup KERNEL32 = SymbolLookup.libraryLookup("kernel32.dll", Arena.global());

    private static final MethodHandle CREATE_FILE_MAPPING_W = LINKER.downcallHandle(
            KERNEL32.find("CreateFileMappingW").orElseThrow(),
            FunctionDescriptor.of(
                    ValueLayout.ADDRESS,
                    ValueLayout.ADDRESS,
                    ValueLayout.ADDRESS,
                    ValueLayout.JAVA_INT,
                    ValueLayout.JAVA_INT,
                    ValueLayout.JAVA_INT,
                    ValueLayout.ADDRESS
            )
    );

    private static final MethodHandle MAP_VIEW_OF_FILE = LINKER.downcallHandle(
            KERNEL32.find("MapViewOfFile").orElseThrow(),
            FunctionDescriptor.of(
                    ValueLayout.ADDRESS,
                    ValueLayout.ADDRESS,
                    ValueLayout.JAVA_INT,
                    ValueLayout.JAVA_INT,
                    ValueLayout.JAVA_INT,
                    ValueLayout.JAVA_LONG
            )
    );

    private static final MethodHandle UNMAP_VIEW_OF_FILE = LINKER.downcallHandle(
            KERNEL32.find("UnmapViewOfFile").orElseThrow(),
            FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS)
    );

    private static final MethodHandle CLOSE_HANDLE = LINKER.downcallHandle(
            KERNEL32.find("CloseHandle").orElseThrow(),
            FunctionDescriptor.of(ValueLayout.JAVA_INT, ValueLayout.ADDRESS)
    );

    private final Arena arena;
    private final MemorySegment mappingHandle;
    private final MemorySegment view;
    private boolean closed;

    private Win32SharedMemory(Arena arena, MemorySegment mappingHandle, MemorySegment view) {
        this.arena = arena;
        this.mappingHandle = mappingHandle;
        this.view = view;
    }

    static Win32SharedMemory create(String name, long byteSize) {
        Arena arena = Arena.ofShared();
        try {
            MemorySegment wideName = arena.allocateFrom(name, StandardCharsets.UTF_16LE);
            int sizeHigh = (int) (byteSize >>> 32);
            int sizeLow = (int) (byteSize & 0xFFFFFFFFL);

            MemorySegment handle = (MemorySegment) CREATE_FILE_MAPPING_W.invokeExact(
                    INVALID_HANDLE_VALUE, MemorySegment.NULL, PAGE_READWRITE, sizeHigh, sizeLow, wideName);
            if (handle.address() == 0L) {
                arena.close();
                throw new IllegalStateException("CreateFileMappingW failed for " + name);
            }

            MemorySegment rawPtr = (MemorySegment) MAP_VIEW_OF_FILE.invokeExact(
                    handle, FILE_MAP_ALL_ACCESS, 0, 0, byteSize);
            if (rawPtr.address() == 0L) {
                int ignored = (int) CLOSE_HANDLE.invokeExact(handle);
                arena.close();
                throw new IllegalStateException("MapViewOfFile failed for " + name);
            }

            MemorySegment sizedView = rawPtr.reinterpret(byteSize, arena, null);
            return new Win32SharedMemory(arena, handle, sizedView);
        } catch (Throwable failure) {
            if (arena.scope().isAlive()) {
                arena.close();
            }
            throw new IllegalStateException("Failed to initialize Win32 shared memory " + name, failure);
        }
    }

    MemorySegment segment() {
        return view;
    }

    @Override
    public void close() {
        if (closed) {
            return;
        }
        closed = true;
        try {
            int ignoredUnmap = (int) UNMAP_VIEW_OF_FILE.invokeExact(view);
            int ignoredClose = (int) CLOSE_HANDLE.invokeExact(mappingHandle);
        } catch (Throwable ignored) {
        } finally {
            arena.close();
        }
    }
}
