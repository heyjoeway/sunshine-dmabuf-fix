// Self-test: does a compositor-style scanout buffer survive Sunshine's
// DMA-BUF -> Vulkan import on this system?
//
// Allocates a buffer the way mutter/KWin do on virtio-gpu (GBM, SCANOUT,
// implicit modifier), writes a known pattern with the CPU, imports it into
// Vulkan exactly like Sunshine's vulkan_encode.cpp (VK_IMAGE_TILING_LINEAR,
// no explicit layout, no dedicated allocation), copies it back and compares.
// Run it with and without the layer to tell whether the bug is present and
// whether the layer fixes it.
//
// libgbm and libvulkan are loaded with dlopen so building needs only a C
// compiler and the bundled Vulkan-Headers.
//
// Prints one line "RESULT=<word> <detail>" and exits with:
//   0 PASS        imported image matches the pattern
//   1 FAIL        imported image is garbled
//   2 ERROR       could not run (no GPU, no GBM, no Vulkan, ...)
//   3 NOT_NEEDED  this system isn't affected (not RADV on virtio_gpu, or
//                 buffers carry an explicit modifier, so Sunshine takes the
//                 modifier path)
#define _GNU_SOURCE
#define VK_NO_PROTOTYPES
#include <dlfcn.h>
#include <fcntl.h>
#include <glob.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <vulkan/vulkan.h>

#define W 1920
#define H 1080
#define DRM_FORMAT_XRGB8888 0x34325258u
#define DRM_FORMAT_MOD_INVALID 0x00ffffffffffffffULL
#define GBM_BO_USE_SCANOUT (1 << 0)
#define GBM_BO_USE_RENDERING (1 << 2)
#define GBM_BO_TRANSFER_WRITE (1 << 1)

struct drm_version_abi {
  int version_major, version_minor, version_patchlevel;
  size_t name_len;
  char *name;
  size_t date_len;
  char *date;
  size_t desc_len;
  char *desc;
};
#define DRM_IOCTL_VERSION_ABI _IOWR('d', 0x00, struct drm_version_abi)

static struct {
  void *(*create_device)(int);
  void *(*bo_create)(void *, uint32_t, uint32_t, uint32_t, uint32_t);
  uint64_t (*bo_get_modifier)(void *);
  uint32_t (*bo_get_stride)(void *);
  void *(*bo_map)(void *, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t *, void **);
  void (*bo_unmap)(void *, void *);
  int (*bo_get_fd)(void *);
} gbm;

#define VK_FUNCS(X)                     \
  X(vkCreateInstance)                   \
  X(vkEnumeratePhysicalDevices)         \
  X(vkGetPhysicalDeviceProperties2)     \
  X(vkGetPhysicalDeviceMemoryProperties) \
  X(vkEnumerateDeviceExtensionProperties) \
  X(vkCreateDevice)                     \
  X(vkGetDeviceQueue)                   \
  X(vkCreateImage)                      \
  X(vkGetImageMemoryRequirements)       \
  X(vkAllocateMemory)                   \
  X(vkBindImageMemory)                  \
  X(vkCreateBuffer)                     \
  X(vkGetBufferMemoryRequirements)      \
  X(vkBindBufferMemory)                 \
  X(vkCreateCommandPool)                \
  X(vkAllocateCommandBuffers)           \
  X(vkBeginCommandBuffer)               \
  X(vkCmdPipelineBarrier)               \
  X(vkCmdCopyImageToBuffer)             \
  X(vkEndCommandBuffer)                 \
  X(vkQueueSubmit)                      \
  X(vkQueueWaitIdle)                    \
  X(vkMapMemory)                        \
  X(vkInvalidateMappedMemoryRanges)     \
  X(vkGetMemoryFdPropertiesKHR)
#define DECL(fn) static PFN_##fn fn;
VK_FUNCS(DECL)

static void result(int code, const char *word, const char *fmt, ...) {
  va_list ap;
  printf("RESULT=%s ", word);
  va_start(ap, fmt);
  vprintf(fmt, ap);
  va_end(ap);
  printf("\n");
  exit(code);
}

static uint32_t pat(int x, int y) { return 0xff000000u | ((x * 7 + y * 3) & 0xff) << 16 | ((x >> 3) & 0xff) << 8 | (y & 0xff); }

static int drm_name(int fd, char *name, size_t len) {
  struct drm_version_abi v = {.name_len = len - 1, .name = name};
  memset(name, 0, len);
  return ioctl(fd, DRM_IOCTL_VERSION_ABI, &v);
}

static int find_mem(VkPhysicalDevice pd, uint32_t bits, VkMemoryPropertyFlags want) {
  VkPhysicalDeviceMemoryProperties mp;
  vkGetPhysicalDeviceMemoryProperties(pd, &mp);
  for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
    if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
  for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
    if (bits & (1u << i)) return i;
  return 0;
}

#define CK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) result(2, "ERROR", "%s failed: %d", #x, r_); } while (0)

int main(void) {
  // Find the virtio_gpu render node.
  glob_t g;
  int drm = -1;
  char name[64];
  if (glob("/dev/dri/renderD*", 0, NULL, &g) == 0) {
    for (size_t i = 0; i < g.gl_pathc && drm < 0; i++) {
      int fd = open(g.gl_pathv[i], O_RDWR | O_CLOEXEC);
      if (fd >= 0 && drm_name(fd, name, sizeof(name)) == 0 && !strcmp(name, "virtio_gpu")) drm = fd;
      else if (fd >= 0) close(fd);
    }
    globfree(&g);
  }
  if (drm < 0) result(3, "NOT_NEEDED", "no virtio_gpu render node (not a native-context VM)");

  void *libgbm = dlopen("libgbm.so.1", RTLD_NOW);
  void *libvk = dlopen("libvulkan.so.1", RTLD_NOW);
  if (!libgbm || !libvk) result(2, "ERROR", "cannot load %s", !libgbm ? "libgbm.so.1" : "libvulkan.so.1");
  gbm.create_device = dlsym(libgbm, "gbm_create_device");
  gbm.bo_create = dlsym(libgbm, "gbm_bo_create");
  gbm.bo_get_modifier = dlsym(libgbm, "gbm_bo_get_modifier");
  gbm.bo_get_stride = dlsym(libgbm, "gbm_bo_get_stride");
  gbm.bo_map = dlsym(libgbm, "gbm_bo_map");
  gbm.bo_unmap = dlsym(libgbm, "gbm_bo_unmap");
  gbm.bo_get_fd = dlsym(libgbm, "gbm_bo_get_fd");
  PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr) dlsym(libvk, "vkGetInstanceProcAddr");
  if (!gbm.bo_map || !gipa) result(2, "ERROR", "missing GBM or Vulkan entry points");

  // Compositor-style scanout buffer with an implicit modifier.
  void *gdev = gbm.create_device(drm);
  void *bo = gdev ? gbm.bo_create(gdev, W, H, DRM_FORMAT_XRGB8888, GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING) : NULL;
  if (!bo) result(2, "ERROR", "GBM could not allocate a scanout buffer (is native context Mesa installed?)");
  uint64_t modifier = gbm.bo_get_modifier(bo);
  if (modifier != DRM_FORMAT_MOD_INVALID)
    result(3, "NOT_NEEDED", "scanout buffers carry explicit modifier 0x%llx, Sunshine uses the modifier path", (unsigned long long) modifier);
  uint32_t stride, mstride;
  void *mdata = NULL;
  stride = gbm.bo_get_stride(bo);
  uint8_t *m = gbm.bo_map(bo, 0, 0, W, H, GBM_BO_TRANSFER_WRITE, &mstride, &mdata);
  if (!m) result(2, "ERROR", "gbm_bo_map failed");
  for (int y = 0; y < H; y++)
    for (int x = 0; x < W; x++) ((uint32_t *) (m + (size_t) y * mstride))[x] = pat(x, y);
  gbm.bo_unmap(bo, mdata);
  int fd = gbm.bo_get_fd(bo);

  // Vulkan device: the RADV device on this virtio_gpu node.
  vkCreateInstance = (PFN_vkCreateInstance) gipa(VK_NULL_HANDLE, "vkCreateInstance");
  VkApplicationInfo ai = {VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "sunshine-dmabuf-selftest", .apiVersion = VK_API_VERSION_1_2};
  VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &ai};
  VkInstance inst;
  CK(vkCreateInstance(&ici, NULL, &inst));
#define LOAD(fn) fn = (PFN_##fn) gipa(inst, #fn);
  VK_FUNCS(LOAD)

  uint32_t n = 16;
  VkPhysicalDevice pds[16], pd = VK_NULL_HANDLE;
  vkEnumeratePhysicalDevices(inst, &n, pds);
  VkPhysicalDeviceDriverProperties drv = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
  for (uint32_t i = 0; i < n && pd == VK_NULL_HANDLE; i++) {
    VkPhysicalDeviceDriverProperties d = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    VkPhysicalDeviceProperties2 p = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &d};
    vkGetPhysicalDeviceProperties2(pds[i], &p);
    if (d.driverID == VK_DRIVER_ID_MESA_RADV) {
      pd = pds[i];
      drv = d;
    }
  }
  if (pd == VK_NULL_HANDLE) result(3, "NOT_NEEDED", "no RADV device (Mesa without amdgpu-virtio, or not an AMD native context)");

  float prio = 1;
  VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = 0, .queueCount = 1, .pQueuePriorities = &prio};
  const char *exts[] = {"VK_KHR_external_memory_fd", "VK_EXT_external_memory_dma_buf"};
  VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci, .enabledExtensionCount = 2, .ppEnabledExtensionNames = exts};
  VkDevice dev;
  CK(vkCreateDevice(pd, &dci, NULL, &dev));
#define LOADD(fn) { PFN_vkVoidFunction f_ = ((PFN_vkGetDeviceProcAddr) gipa(inst, "vkGetDeviceProcAddr"))(dev, #fn); if (f_) fn = (PFN_##fn) f_; }
  VK_FUNCS(LOADD)
  VkQueue q;
  vkGetDeviceQueue(dev, 0, 0, &q);

  // Import exactly like Sunshine does when the modifier is DRM_FORMAT_MOD_INVALID.
  VkExternalMemoryImageCreateInfo ext = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, NULL, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
  VkImageCreateInfo ic = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &ext, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_B8G8R8A8_UNORM,
    .extent = {W, H, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
    .tiling = VK_IMAGE_TILING_LINEAR, .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
  VkImage img;
  CK(vkCreateImage(dev, &ic, NULL, &img));
  VkMemoryRequirements mr;
  vkGetImageMemoryRequirements(dev, img, &mr);
  VkMemoryFdPropertiesKHR fdp = {VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
  vkGetMemoryFdPropertiesKHR(dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, fd, &fdp);
  VkImportMemoryFdInfoKHR imp = {VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR, NULL, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, fd};
  VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &imp, mr.size,
    find_mem(pd, fdp.memoryTypeBits ? fdp.memoryTypeBits : mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
  VkDeviceMemory mem;
  CK(vkAllocateMemory(dev, &mai, NULL, &mem));
  CK(vkBindImageMemory(dev, img, mem, 0));

  // Copy back to a host buffer.
  VkBufferCreateInfo bc = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = (VkDeviceSize) W * H * 4, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
  VkBuffer buf;
  CK(vkCreateBuffer(dev, &bc, NULL, &buf));
  VkMemoryRequirements bmr;
  vkGetBufferMemoryRequirements(dev, buf, &bmr);
  VkMemoryAllocateInfo bai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, bmr.size,
    find_mem(pd, bmr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT)};
  VkDeviceMemory bmem;
  CK(vkAllocateMemory(dev, &bai, NULL, &bmem));
  CK(vkBindBufferMemory(dev, buf, bmem, 0));

  VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = 0};
  VkCommandPool pool;
  CK(vkCreateCommandPool(dev, &pci, NULL, &pool));
  VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
  VkCommandBuffer cb;
  CK(vkAllocateCommandBuffers(dev, &cai, &cb));
  VkCommandBufferBeginInfo cbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  vkBeginCommandBuffer(cb, &cbi);
  VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
    .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT, .dstQueueFamilyIndex = 0, .image = img,
    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
  vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
  VkBufferImageCopy reg = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {W, H, 1}};
  vkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &reg);
  vkEndCommandBuffer(cb);
  VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};
  CK(vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE));
  CK(vkQueueWaitIdle(q));

  uint32_t *px;
  CK(vkMapMemory(dev, bmem, 0, VK_WHOLE_SIZE, 0, (void **) &px));
  VkMappedMemoryRange rng = {VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, NULL, bmem, 0, VK_WHOLE_SIZE};
  vkInvalidateMappedMemoryRanges(dev, 1, &rng);
  long bad = 0;
  for (int y = 0; y < H; y++)
    for (int x = 0; x < W; x++)
      if ((px[(size_t) y * W + x] | 0xff000000u) != pat(x, y)) bad++;

  // Process exit releases the remaining Vulkan and GBM objects.
  if (bad)
    result(1, "FAIL", "%ld/%d pixels wrong (stride %u, %s)", bad, W * H, stride, drv.driverInfo);
  result(0, "PASS", "import matches (stride %u, %s)", stride, drv.driverInfo);
}
