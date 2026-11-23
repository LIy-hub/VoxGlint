package dev.minecraftue.bridge;

import com.mojang.blaze3d.GpuFormat;
import com.mojang.blaze3d.systems.RenderSystem;
import com.mojang.blaze3d.textures.GpuTexture;
import com.mojang.blaze3d.vulkan.VulkanCommandEncoder;
import com.mojang.blaze3d.vulkan.VulkanDevice;
import com.mojang.blaze3d.vulkan.VulkanGpuTexture;
import com.sun.jna.Pointer;
import com.sun.jna.platform.win32.Kernel32;
import com.sun.jna.platform.win32.WinNT.HANDLE;
import com.sun.jna.platform.win32.WinNT.HANDLEByReference;
import dev.minecraftue.bridge.mixin.GpuDeviceAccessor;
import org.lwjgl.system.JNI;
import org.lwjgl.system.MemoryStack;
import org.lwjgl.vulkan.*;

import java.nio.ByteOrder;

import static org.lwjgl.vulkan.KHRExternalMemoryWin32.vkGetMemoryWin32HandlePropertiesKHR;
import static org.lwjgl.vulkan.VK12.*;

final class VulkanHudInterop implements AutoCloseable {
    private static final int MEMORY_TYPE = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT;
    private static final int FENCE_TYPE = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT;
    private static final int PIXEL_FORMAT = VK_FORMAT_R8G8B8A8_UNORM;
    private final VulkanDevice device;
    private final VkDevice vkDevice;
    private final Slot[] slots = {new Slot(), new Slot(), new Slot()};
    final GpuUiCapture.Adapter adapter;

    VulkanHudInterop(VulkanGpuTexture source) {
        if (source.getFormat() != GpuFormat.RGBA8_UNORM || (source.usage() & GpuTexture.USAGE_COPY_SRC) == 0) {
            throw new IllegalStateException("Vulkan HUD target is not RGBA8 with copy-source usage");
        }
        var backend = ((GpuDeviceAccessor) (Object) RenderSystem.getDevice()).minecraftUe$getBackend();
        if (!(backend instanceof VulkanDevice vulkan)) {
            throw new IllegalStateException("Vulkan HUD texture does not belong to the current Vulkan device");
        }
        device = vulkan;
        vkDevice = device.vkDevice();
        if (!vkDevice.getCapabilities().VK_KHR_external_memory_win32
                || !vkDevice.getCapabilities().VK_KHR_external_semaphore_win32) {
            throw new IllegalStateException("Vulkan external memory and fence extensions are unavailable");
        }
        try (var stack = MemoryStack.stackPush()) {
            var ids = VkPhysicalDeviceIDProperties.calloc(stack).sType$Default();
            var props = VkPhysicalDeviceProperties2.calloc(stack).sType$Default().pNext(ids.address());
            var physical = vkDevice.getPhysicalDevice();
            vkGetPhysicalDeviceProperties2(physical, props);
            if (!ids.deviceLUIDValid() || ids.deviceNodeMask() == 0) {
                throw new IllegalStateException("Vulkan device has no D3D12 adapter LUID");
            }
            var luid = ids.deviceLUID().order(ByteOrder.LITTLE_ENDIAN);
            adapter = new GpuUiCapture.Adapter(luid.getInt(0), luid.getInt(4), ids.deviceNodeMask());

            var externalImage = VkPhysicalDeviceExternalImageFormatInfo.calloc(stack).sType$Default().handleType(MEMORY_TYPE);
            var format = VkPhysicalDeviceImageFormatInfo2.calloc(stack).sType$Default().pNext(externalImage.address())
                    .format(PIXEL_FORMAT).type(VK_IMAGE_TYPE_2D).tiling(VK_IMAGE_TILING_OPTIMAL).usage(VK_IMAGE_USAGE_TRANSFER_DST_BIT);
            var memory = VkExternalImageFormatProperties.calloc(stack).sType$Default();
            var formatProps = VkImageFormatProperties2.calloc(stack).sType$Default().pNext(memory.address());
            check(vkGetPhysicalDeviceImageFormatProperties2(physical, format, formatProps), "querying D3D12 image support");
            if ((memory.externalMemoryProperties().externalMemoryFeatures() & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) == 0
                    || source.getWidth(0) > formatProps.imageFormatProperties().maxExtent().width()
                    || source.getHeight(0) > formatProps.imageFormatProperties().maxExtent().height()) {
                throw new IllegalStateException("Vulkan device cannot import this D3D12 texture");
            }

            var timeline = VkSemaphoreTypeCreateInfo.calloc(stack).sType$Default().semaphoreType(VK_SEMAPHORE_TYPE_TIMELINE);
            var fence = VkPhysicalDeviceExternalSemaphoreInfo.calloc(stack).sType$Default()
                    .pNext(timeline.address()).handleType(FENCE_TYPE);
            var fenceProps = VkExternalSemaphoreProperties.calloc(stack).sType$Default();
            vkGetPhysicalDeviceExternalSemaphoreProperties(physical, fence, fenceProps);
            if ((fenceProps.externalSemaphoreFeatures() & VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT) == 0) {
                throw new IllegalStateException("Vulkan device cannot import a D3D12 timeline fence");
            }
        }
    }

    void importObjects(int width, int height, int pid, long[] textures, long[] fences) {
        if (pid <= 0) {
            throw new IllegalStateException("Renderer did not publish Vulkan texture handles");
        }
        var kernel = Kernel32.INSTANCE;
        var remote = kernel.OpenProcess(0x40, false, pid); // PROCESS_DUP_HANDLE
        if (remote == null || remote.getPointer() == null) {
            throw new IllegalStateException("Cannot open renderer shared-handle table (Win32 " + kernel.GetLastError() + ")");
        }
        try {
            for (int index = 0; index < slots.length; ++index) {
                HANDLE texture = duplicate(remote, textures[index]);
                HANDLE fence = null;
                try {
                    fence = duplicate(remote, fences[index]);
                    importSlot(slots[index], width, height,
                            Pointer.nativeValue(texture.getPointer()), Pointer.nativeValue(fence.getPointer()));
                } finally {
                    // Importing NT handles does not transfer ownership to Vulkan.
                    if (fence != null) {
                        kernel.CloseHandle(fence);
                    }
                    kernel.CloseHandle(texture);
                }
            }
        } finally {
            kernel.CloseHandle(remote);
        }
    }

    private static HANDLE duplicate(HANDLE remote, long handle) {
        var kernel = Kernel32.INSTANCE;
        var duplicate = new HANDLEByReference();
        if (handle == 0 || !kernel.DuplicateHandle(remote, new HANDLE(Pointer.createConstant(handle)),
                kernel.GetCurrentProcess(), duplicate, 0, false, 2)) { // DUPLICATE_SAME_ACCESS
            throw new IllegalStateException("Cannot duplicate the shared HUD handle (Win32 " + kernel.GetLastError() + ")");
        }
        return duplicate.getValue();
    }

    private void importSlot(Slot slot, int width, int height, long handle, long fence) {
        try (var stack = MemoryStack.stackPush()) {
            var output = stack.mallocLong(1);
            var externalImage = VkExternalMemoryImageCreateInfo.calloc(stack).sType$Default().handleTypes(MEMORY_TYPE);
            var image = VkImageCreateInfo.calloc(stack).sType$Default().pNext(externalImage.address())
                    .imageType(VK_IMAGE_TYPE_2D).format(PIXEL_FORMAT).mipLevels(1).arrayLayers(1)
                    .samples(VK_SAMPLE_COUNT_1_BIT).tiling(VK_IMAGE_TILING_OPTIMAL).usage(VK_IMAGE_USAGE_TRANSFER_DST_BIT)
                    .sharingMode(VK_SHARING_MODE_EXCLUSIVE).initialLayout(VK_IMAGE_LAYOUT_UNDEFINED);
            image.extent().set(width, height, 1);
            check(vkCreateImage(vkDevice, image, null, output), "creating Vulkan HUD image");
            slot.image = output.get(0);

            var requirements = VkMemoryRequirements.calloc(stack);
            vkGetImageMemoryRequirements(vkDevice, slot.image, requirements);
            var imported = VkMemoryWin32HandlePropertiesKHR.calloc(stack).sType$Default();
            check(vkGetMemoryWin32HandlePropertiesKHR(vkDevice, MEMORY_TYPE, handle, imported), "querying D3D12 memory type");
            int typeBits = requirements.memoryTypeBits() & imported.memoryTypeBits();
            var memory = VkPhysicalDeviceMemoryProperties.calloc(stack);
            vkGetPhysicalDeviceMemoryProperties(vkDevice.getPhysicalDevice(), memory);
            int type = 0;
            while (type < memory.memoryTypeCount() && ((typeBits & (1 << type)) == 0
                    || (memory.memoryTypes(type).propertyFlags() & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) == 0)) {
                ++type;
            }
            if (type == memory.memoryTypeCount()) {
                throw new IllegalStateException("Vulkan and D3D12 HUD images have no common device-local memory type");
            }
            var dedicated = VkMemoryDedicatedAllocateInfo.calloc(stack).sType$Default().image(slot.image);
            var importMemory = VkImportMemoryWin32HandleInfoKHR.calloc(stack).sType$Default().pNext(dedicated.address())
                    .handleType(MEMORY_TYPE).handle(handle);
            var allocation = VkMemoryAllocateInfo.calloc(stack).sType$Default().pNext(importMemory.address())
                    .allocationSize(requirements.size()).memoryTypeIndex(type);
            check(vkAllocateMemory(vkDevice, allocation, null, output), "importing D3D12 HUD memory");
            slot.memory = output.get(0);
            check(vkBindImageMemory(vkDevice, slot.image, slot.memory, 0), "binding D3D12 HUD memory");

            var timeline = VkSemaphoreTypeCreateInfo.calloc(stack).sType$Default().semaphoreType(VK_SEMAPHORE_TYPE_TIMELINE);
            var semaphore = VkSemaphoreCreateInfo.calloc(stack).sType$Default().pNext(timeline.address());
            check(vkCreateSemaphore(vkDevice, semaphore, null, output), "creating Vulkan HUD semaphore");
            slot.semaphore = output.get(0);
            var importFence = VkImportSemaphoreWin32HandleInfoKHR.calloc(stack).sType$Default().semaphore(slot.semaphore)
                    .handleType(FENCE_TYPE).handle(fence);
            long importSemaphore = vkDevice.getCapabilities().vkImportSemaphoreWin32HandleKHR;
            if (importSemaphore == 0) {
                throw new IllegalStateException("Vulkan D3D12 fence import entry point is unavailable");
            }
            // LWJGL 3.4.1 requires both fields, but Vulkan VUID 01469 requires a null name when a HANDLE is set.
            check(JNI.callPPI(vkDevice.address(), importFence.address(), importSemaphore), "importing D3D12 HUD fence");
        }
    }

    void copy(VulkanGpuTexture source, int index, long previous, long next, int width, int height) {
        Slot slot = slots[index];
        var encoder = device.createCommandEncoder();
        var commands = encoder.allocateAndBeginTransientCommandBuffer();
        int queue = device.graphicsQueue().queueFamilyIndex();

        try (var stack = MemoryStack.stackPush()) {
            // The Minecraft encoder uses GENERAL for all of its image operations.
            VulkanCommandEncoder.memoryBarrier(commands, stack);
            var barrier = VkImageMemoryBarrier.calloc(1, stack).sType$Default().image(slot.image)
                    .oldLayout(previous == 0 ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL)
                    .newLayout(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)
                    .srcQueueFamilyIndex(VK_QUEUE_FAMILY_EXTERNAL).dstQueueFamilyIndex(queue)
                    .dstAccessMask(VK_ACCESS_TRANSFER_WRITE_BIT);
            barrier.subresourceRange().set(VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1);
            vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    0, null, null, barrier);
            var region = VkImageCopy.calloc(1, stack);
            region.srcSubresource().set(VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1);
            region.dstSubresource().set(VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1);
            region.extent().set(width, height, 1);
            vkCmdCopyImage(commands, source.vkImage(), VK_IMAGE_LAYOUT_GENERAL, slot.image,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, region);
            barrier.oldLayout(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL).newLayout(VK_IMAGE_LAYOUT_GENERAL)
                    .srcQueueFamilyIndex(queue).dstQueueFamilyIndex(VK_QUEUE_FAMILY_EXTERNAL)
                    .srcAccessMask(VK_ACCESS_TRANSFER_WRITE_BIT).dstAccessMask(0);
            vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                    0, null, null, barrier);
            VulkanCommandEncoder.memoryBarrier(commands, stack);
        }
        check(vkEndCommandBuffer(commands), "recording shared Vulkan HUD copy");
        if (previous != 0) {
            encoder.waitSemaphore(slot.semaphore, 2 * previous, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
        }
        encoder.execute(commands);
        encoder.signalSemaphore(slot.semaphore, 2 * next - 1, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
        // Minecraft submits this recorded copy with the rest of the frame.
    }

    @Override
    public void close() {
        // Minecraft's destruction queue is retired only after the matching graphics submission completes.
        device.createCommandEncoder().queueForDestroy(() -> {
            for (Slot slot : slots) {
                if (slot.semaphore != 0) vkDestroySemaphore(vkDevice, slot.semaphore, null);
                if (slot.image != 0) vkDestroyImage(vkDevice, slot.image, null);
                if (slot.memory != 0) vkFreeMemory(vkDevice, slot.memory, null);
            }
        });
    }

    private static void check(int result, String action) {
        if (result != VK_SUCCESS) {
            throw new IllegalStateException(action + " failed (VkResult " + result + ")");
        }
    }

    private static final class Slot {
        long image;
        long memory;
        long semaphore;
    }
}
