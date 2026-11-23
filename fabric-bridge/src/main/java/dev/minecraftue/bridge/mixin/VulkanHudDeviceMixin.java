package dev.minecraftue.bridge.mixin;

import com.mojang.blaze3d.vulkan.VulkanBackend;
import com.mojang.blaze3d.vulkan.VulkanPhysicalDevice;
import org.lwjgl.system.Platform;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.ModifyVariable;

import java.util.Collection;
import java.util.LinkedHashSet;
import java.util.Set;

import static org.lwjgl.vulkan.KHRExternalMemoryWin32.VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME;
import static org.lwjgl.vulkan.KHRExternalSemaphoreWin32.VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME;

@Mixin(VulkanBackend.class)
abstract class VulkanHudDeviceMixin {
    @ModifyVariable(method = "createDevice(Ljava/util/Collection;Lcom/mojang/blaze3d/vulkan/VulkanPhysicalDevice;Ljava/util/Set;)Lorg/lwjgl/vulkan/VkDevice;",
            at = @At("HEAD"), argsOnly = true, ordinal = 0)
    private static Collection<String> minecraftUe$enableInterop(Collection<String> original,
            Collection<String> extensions, VulkanPhysicalDevice physical, Set<?> features) {
        if (Platform.get() != Platform.WINDOWS || Boolean.getBoolean("minecraftue.cpuHud")
                || !physical.hasDeviceExtension(VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME)
                || !physical.hasDeviceExtension(VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME)) {
            return original;
        }
        var result = new LinkedHashSet<>(original);
        result.add(VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME);
        result.add(VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME);
        return result;
    }
}
