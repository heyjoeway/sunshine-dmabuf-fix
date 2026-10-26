// VK_LAYER_LOCAL_dmabuf_dedicated
//
// Works around Sunshine's Vulkan encoder importing a DMA-BUF that has no DRM
// format modifier as VK_IMAGE_TILING_LINEAR without a dedicated allocation.
// Under amdgpu native context (virtio-gpu), compositor scanout buffers are
// tiled with an *implicit* modifier: the layout lives only in the BO
// metadata, and RADV reads that metadata only for dedicated imports. This
// layer chains VkMemoryDedicatedAllocateInfo onto such imports.
//
// The layer deactivates itself unless all of these hold:
//   - the process is Sunshine (name starts with "sunshine"), or
//     DMABUF_DEDICATED_LAYER_ALL=1 is set (used by the self-test)
//   - the Vulkan driver is RADV
//   - the device's DRM node belongs to virtio_gpu (native context)
// and per import it only acts when the app did not already ask for a
// dedicated allocation (so a fixed Sunshine makes it a no-op), and exactly
// one candidate image is pending. If the app then binds the memory to a
// different image or offset, the layer logs it and disables itself for the
// rest of the process.
//
// Environment:
//   DISABLE_DMABUF_DEDICATED_LAYER=1  loader skips the layer entirely
//   DMABUF_DEDICATED_LAYER_ALL=1      act in any process, not just Sunshine
//   DMABUF_DEDICATED_LAYER_DEBUG=1    log every injected allocation
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <vulkan/vk_layer.h>
#include <vulkan/vulkan.h>

#define EXPORT __attribute__((visibility("default")))
#define MAX_OBJS 64
#define MAX_PENDING 4
#define MAX_INJECTED 8

// Minimal copy of the stable DRM_IOCTL_VERSION ABI, to avoid a libdrm dependency.
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

typedef struct {
  void *key;
  VkInstance instance;
  PFN_vkGetInstanceProcAddr gipa;
  PFN_vkDestroyInstance destroy;
} inst_data;

typedef struct {
  VkDeviceMemory mem;
  VkImage image;
} injected_alloc;

typedef struct {
  void *key;
  int active;  // all device-level gates passed
  PFN_vkGetDeviceProcAddr gdpa;
  PFN_vkDestroyDevice destroy;
  PFN_vkCreateImage create_image;
  PFN_vkDestroyImage destroy_image;
  PFN_vkAllocateMemory allocate_memory;
  PFN_vkFreeMemory free_memory;
  PFN_vkBindImageMemory bind_image_memory;
  PFN_vkBindImageMemory2 bind_image_memory2;
  VkImage pending[MAX_PENDING];  // DMA-BUF images created without a modifier
  injected_alloc injected[MAX_INJECTED];
} dev_data;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static inst_data insts[MAX_OBJS];
static dev_data devs[MAX_OBJS];
static int tripped;  // a safety check failed; stay inert for the rest of the process

static void *key_of(const void *handle) { return *(void **) handle; }

static void log_msg(const char *fmt, const char *arg) {
  fprintf(stderr, "[dmabuf_dedicated] ");
  fprintf(stderr, fmt, arg);
  fputc('\n', stderr);
}

static int debug_enabled(void) { return getenv("DMABUF_DEDICATED_LAYER_DEBUG") != NULL; }

static int process_allowed(void) {
  return getenv("DMABUF_DEDICATED_LAYER_ALL") != NULL || !strncmp(program_invocation_short_name, "sunshine", 8);
}

static inst_data *find_inst(void *key) {
  for (int i = 0; i < MAX_OBJS; i++)
    if (insts[i].key == key) return &insts[i];
  return NULL;
}

static dev_data *find_dev(void *key) {
  for (int i = 0; i < MAX_OBJS; i++)
    if (devs[i].key == key) return &devs[i];
  return NULL;
}

static const void *find_in_chain(const void *pnext, VkStructureType type) {
  for (const VkBaseInStructure *s = pnext; s; s = s->pNext)
    if (s->sType == type) return s;
  return NULL;
}

static int is_multiplanar(VkFormat f) {
  return (f >= VK_FORMAT_G8B8G8R8_422_UNORM && f <= VK_FORMAT_G16_B16_R16_3PLANE_444_UNORM) ||
         (f >= VK_FORMAT_G8_B8R8_2PLANE_444_UNORM && f <= VK_FORMAT_G16_B16R16_2PLANE_444_UNORM);
}

// Returns 1 if the render node major:minor is driven by virtio_gpu.
static int node_is_virtio_gpu(int64_t major, int64_t minor) {
  char path[64];
  snprintf(path, sizeof(path), "/dev/char/%lld:%lld", (long long) major, (long long) minor);
  int fd = open(path, O_RDWR | O_CLOEXEC);
  if (fd < 0) {
    snprintf(path, sizeof(path), "/dev/dri/renderD%lld", (long long) minor);
    fd = open(path, O_RDWR | O_CLOEXEC);
  }
  if (fd < 0) return 0;
  char name[64] = {0};
  struct drm_version_abi v = {.name_len = sizeof(name) - 1, .name = name};
  int ok = ioctl(fd, DRM_IOCTL_VERSION_ABI, &v) == 0 && !strcmp(name, "virtio_gpu");
  close(fd);
  return ok;
}

// Device-level gates: RADV on a virtio_gpu DRM node.
static int device_qualifies(inst_data *inst, VkPhysicalDevice pd) {
  PFN_vkEnumerateDeviceExtensionProperties enum_ext =
    (PFN_vkEnumerateDeviceExtensionProperties) inst->gipa(inst->instance, "vkEnumerateDeviceExtensionProperties");
  PFN_vkGetPhysicalDeviceProperties2 props2 =
    (PFN_vkGetPhysicalDeviceProperties2) inst->gipa(inst->instance, "vkGetPhysicalDeviceProperties2");
  if (!enum_ext || !props2) return 0;

  uint32_t n = 0;
  enum_ext(pd, NULL, &n, NULL);
  VkExtensionProperties *exts = calloc(n ? n : 1, sizeof(*exts));
  enum_ext(pd, NULL, &n, exts);
  int has_drm = 0;
  for (uint32_t i = 0; i < n; i++)
    if (!strcmp(exts[i].extensionName, VK_EXT_PHYSICAL_DEVICE_DRM_EXTENSION_NAME)) has_drm = 1;
  free(exts);
  if (!has_drm) return 0;

  VkPhysicalDeviceDrmPropertiesEXT drm = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT};
  VkPhysicalDeviceDriverProperties drv = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES, .pNext = &drm};
  VkPhysicalDeviceProperties2 p = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &drv};
  props2(pd, &p);
  if (drv.driverID != VK_DRIVER_ID_MESA_RADV) return 0;
  if (!drm.hasRender) return 0;
  return node_is_virtio_gpu(drm.renderMajor, drm.renderMinor);
}

static VKAPI_ATTR VkResult VKAPI_CALL layer_CreateInstance(const VkInstanceCreateInfo *ci, const VkAllocationCallbacks *alloc, VkInstance *out) {
  VkLayerInstanceCreateInfo *lci = (VkLayerInstanceCreateInfo *) ci->pNext;
  while (lci && !(lci->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO && lci->function == VK_LAYER_LINK_INFO))
    lci = (VkLayerInstanceCreateInfo *) lci->pNext;
  if (!lci) return VK_ERROR_INITIALIZATION_FAILED;
  PFN_vkGetInstanceProcAddr gipa = lci->u.pLayerInfo->pfnNextGetInstanceProcAddr;
  lci->u.pLayerInfo = lci->u.pLayerInfo->pNext;

  PFN_vkCreateInstance next_create = (PFN_vkCreateInstance) gipa(VK_NULL_HANDLE, "vkCreateInstance");
  VkResult res = next_create(ci, alloc, out);
  if (res != VK_SUCCESS) return res;

  pthread_mutex_lock(&lock);
  inst_data *d = find_inst(NULL);
  if (d) {
    d->key = key_of(*out);
    d->instance = *out;
    d->gipa = gipa;
    d->destroy = (PFN_vkDestroyInstance) gipa(*out, "vkDestroyInstance");
  }
  pthread_mutex_unlock(&lock);
  return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL layer_DestroyInstance(VkInstance inst, const VkAllocationCallbacks *alloc) {
  pthread_mutex_lock(&lock);
  inst_data *d = find_inst(key_of(inst));
  PFN_vkDestroyInstance destroy = d ? d->destroy : NULL;
  if (d) memset(d, 0, sizeof(*d));
  pthread_mutex_unlock(&lock);
  if (destroy) destroy(inst, alloc);
}

static VKAPI_ATTR VkResult VKAPI_CALL layer_CreateDevice(VkPhysicalDevice pd, const VkDeviceCreateInfo *ci, const VkAllocationCallbacks *alloc, VkDevice *out) {
  VkLayerDeviceCreateInfo *lci = (VkLayerDeviceCreateInfo *) ci->pNext;
  while (lci && !(lci->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO && lci->function == VK_LAYER_LINK_INFO))
    lci = (VkLayerDeviceCreateInfo *) lci->pNext;
  if (!lci) return VK_ERROR_INITIALIZATION_FAILED;
  PFN_vkGetInstanceProcAddr gipa = lci->u.pLayerInfo->pfnNextGetInstanceProcAddr;
  PFN_vkGetDeviceProcAddr gdpa = lci->u.pLayerInfo->pfnNextGetDeviceProcAddr;
  lci->u.pLayerInfo = lci->u.pLayerInfo->pNext;

  PFN_vkCreateDevice next_create = (PFN_vkCreateDevice) gipa(VK_NULL_HANDLE, "vkCreateDevice");
  VkResult res = next_create(pd, ci, alloc, out);
  if (res != VK_SUCCESS) return res;

  pthread_mutex_lock(&lock);
  inst_data *inst = find_inst(key_of(pd));
  int active = !tripped && process_allowed() && inst && device_qualifies(inst, pd);
  dev_data *d = find_dev(NULL);
  if (d) {
    memset(d, 0, sizeof(*d));
    d->key = key_of(*out);
    d->active = active;
    d->gdpa = gdpa;
    d->destroy = (PFN_vkDestroyDevice) gdpa(*out, "vkDestroyDevice");
    d->create_image = (PFN_vkCreateImage) gdpa(*out, "vkCreateImage");
    d->destroy_image = (PFN_vkDestroyImage) gdpa(*out, "vkDestroyImage");
    d->allocate_memory = (PFN_vkAllocateMemory) gdpa(*out, "vkAllocateMemory");
    d->free_memory = (PFN_vkFreeMemory) gdpa(*out, "vkFreeMemory");
    d->bind_image_memory = (PFN_vkBindImageMemory) gdpa(*out, "vkBindImageMemory");
    d->bind_image_memory2 = (PFN_vkBindImageMemory2) gdpa(*out, "vkBindImageMemory2");
  }
  pthread_mutex_unlock(&lock);
  if (active) log_msg("active: RADV on virtio_gpu (native context)%s", "");
  return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL layer_DestroyDevice(VkDevice dev, const VkAllocationCallbacks *alloc) {
  pthread_mutex_lock(&lock);
  dev_data *d = find_dev(key_of(dev));
  PFN_vkDestroyDevice destroy = d ? d->destroy : NULL;
  if (d) memset(d, 0, sizeof(*d));
  pthread_mutex_unlock(&lock);
  if (destroy) destroy(dev, alloc);
}

static VKAPI_ATTR VkResult VKAPI_CALL layer_CreateImage(VkDevice dev, const VkImageCreateInfo *ci, const VkAllocationCallbacks *alloc, VkImage *out) {
  pthread_mutex_lock(&lock);
  dev_data *d = find_dev(key_of(dev));
  PFN_vkCreateImage next = d->create_image;
  pthread_mutex_unlock(&lock);

  VkResult res = next(dev, ci, alloc, out);
  if (res != VK_SUCCESS) return res;

  // Same restrictions as RADV's metadata path: one plane, one sample, no mips/layers.
  const VkExternalMemoryImageCreateInfo *ext = find_in_chain(ci->pNext, VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO);
  if (ext && (ext->handleTypes & VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT) &&
      ci->tiling != VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT && ci->samples == VK_SAMPLE_COUNT_1_BIT &&
      ci->mipLevels == 1 && ci->arrayLayers == 1 && !is_multiplanar(ci->format)) {
    pthread_mutex_lock(&lock);
    if (d->active && !tripped) {
      for (int i = 0; i < MAX_PENDING; i++)
        if (d->pending[i] == VK_NULL_HANDLE) {
          d->pending[i] = *out;
          break;
        }
    }
    pthread_mutex_unlock(&lock);
  }
  return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL layer_DestroyImage(VkDevice dev, VkImage img, const VkAllocationCallbacks *alloc) {
  pthread_mutex_lock(&lock);
  dev_data *d = find_dev(key_of(dev));
  for (int i = 0; i < MAX_PENDING; i++)
    if (d->pending[i] == img) d->pending[i] = VK_NULL_HANDLE;
  PFN_vkDestroyImage next = d->destroy_image;
  pthread_mutex_unlock(&lock);
  next(dev, img, alloc);
}

static VKAPI_ATTR VkResult VKAPI_CALL layer_AllocateMemory(VkDevice dev, const VkMemoryAllocateInfo *ai, const VkAllocationCallbacks *alloc, VkDeviceMemory *out) {
  pthread_mutex_lock(&lock);
  dev_data *d = find_dev(key_of(dev));
  PFN_vkAllocateMemory next = d->allocate_memory;
  VkImage img = VK_NULL_HANDLE;
  const VkImportMemoryFdInfoKHR *imp = find_in_chain(ai->pNext, VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR);
  if (d->active && !tripped && imp && imp->handleType == VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT &&
      !find_in_chain(ai->pNext, VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO)) {
    // Only act when the target image is unambiguous.
    int count = 0, idx = -1;
    for (int i = 0; i < MAX_PENDING; i++)
      if (d->pending[i] != VK_NULL_HANDLE) {
        count++;
        idx = i;
      }
    if (count == 1) {
      img = d->pending[idx];
      d->pending[idx] = VK_NULL_HANDLE;
    } else if (count > 1) {
      for (int i = 0; i < MAX_PENDING; i++) d->pending[i] = VK_NULL_HANDLE;
      log_msg("skipped: %s DMA-BUF images pending, target image is ambiguous", "multiple");
    }
  }
  pthread_mutex_unlock(&lock);

  if (img == VK_NULL_HANDLE) return next(dev, ai, alloc, out);

  VkMemoryDedicatedAllocateInfo ded = {
    .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
    .pNext = ai->pNext,
    .image = img,
  };
  VkMemoryAllocateInfo patched = *ai;
  patched.pNext = &ded;
  VkResult res = next(dev, &patched, alloc, out);
  if (res != VK_SUCCESS) return res;

  if (debug_enabled()) log_msg("added dedicated allocation to %s", "DMA-BUF import");
  pthread_mutex_lock(&lock);
  for (int i = 0; i < MAX_INJECTED; i++)
    if (d->injected[i].mem == VK_NULL_HANDLE) {
      d->injected[i] = (injected_alloc) {*out, img};
      break;
    }
  pthread_mutex_unlock(&lock);
  return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL layer_FreeMemory(VkDevice dev, VkDeviceMemory mem, const VkAllocationCallbacks *alloc) {
  pthread_mutex_lock(&lock);
  dev_data *d = find_dev(key_of(dev));
  for (int i = 0; i < MAX_INJECTED; i++)
    if (d->injected[i].mem == mem) d->injected[i] = (injected_alloc) {0};
  PFN_vkFreeMemory next = d->free_memory;
  pthread_mutex_unlock(&lock);
  next(dev, mem, alloc);
}

// Verify the app bound an injected allocation the way we assumed; trip otherwise.
static void check_bind(dev_data *d, VkImage img, VkDeviceMemory mem, VkDeviceSize offset) {
  for (int i = 0; i < MAX_INJECTED; i++)
    if (d->injected[i].mem == mem && mem != VK_NULL_HANDLE) {
      if (d->injected[i].image != img || offset != 0) {
        tripped = 1;
        log_msg("disabled: %s; please report this", "app bound an injected allocation to another image/offset");
      }
      return;
    }
}

static VKAPI_ATTR VkResult VKAPI_CALL layer_BindImageMemory(VkDevice dev, VkImage img, VkDeviceMemory mem, VkDeviceSize offset) {
  pthread_mutex_lock(&lock);
  dev_data *d = find_dev(key_of(dev));
  check_bind(d, img, mem, offset);
  PFN_vkBindImageMemory next = d->bind_image_memory;
  pthread_mutex_unlock(&lock);
  return next(dev, img, mem, offset);
}

static VKAPI_ATTR VkResult VKAPI_CALL layer_BindImageMemory2(VkDevice dev, uint32_t n, const VkBindImageMemoryInfo *infos) {
  pthread_mutex_lock(&lock);
  dev_data *d = find_dev(key_of(dev));
  for (uint32_t i = 0; i < n; i++) check_bind(d, infos[i].image, infos[i].memory, infos[i].memoryOffset);
  PFN_vkBindImageMemory2 next = d->bind_image_memory2;
  pthread_mutex_unlock(&lock);
  return next(dev, n, infos);
}

static PFN_vkVoidFunction intercept_instance(const char *name) {
#define HOOK(fn) if (!strcmp(name, "vk" #fn)) return (PFN_vkVoidFunction) layer_##fn
  HOOK(CreateInstance);
  HOOK(DestroyInstance);
  HOOK(CreateDevice);
#undef HOOK
  return NULL;
}

static PFN_vkVoidFunction intercept_device(const char *name) {
#define HOOK(fn) if (!strcmp(name, "vk" #fn)) return (PFN_vkVoidFunction) layer_##fn
  HOOK(DestroyDevice);
  HOOK(CreateImage);
  HOOK(DestroyImage);
  HOOK(AllocateMemory);
  HOOK(FreeMemory);
  HOOK(BindImageMemory);
  HOOK(BindImageMemory2);
  if (!strcmp(name, "vkBindImageMemory2KHR")) return (PFN_vkVoidFunction) layer_BindImageMemory2;
#undef HOOK
  return NULL;
}

EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL layer_GetDeviceProcAddr(VkDevice dev, const char *name) {
  if (!strcmp(name, "vkGetDeviceProcAddr")) return (PFN_vkVoidFunction) layer_GetDeviceProcAddr;
  pthread_mutex_lock(&lock);
  dev_data *d = find_dev(key_of(dev));
  PFN_vkGetDeviceProcAddr gdpa = d ? d->gdpa : NULL;
  pthread_mutex_unlock(&lock);
  if (!gdpa) return NULL;
  // vkDestroyDevice is always ours (it frees the table slot). Everything else
  // goes straight to the driver unless this device passed the gates, so other
  // apps and devices see no wrappers at all.
  if (!strcmp(name, "vkDestroyDevice")) return (PFN_vkVoidFunction) layer_DestroyDevice;
  PFN_vkVoidFunction next = gdpa(dev, name);
  PFN_vkVoidFunction f = d->active ? intercept_device(name) : NULL;
  return (f && next) ? f : next;
}

EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL layer_GetInstanceProcAddr(VkInstance inst, const char *name) {
  if (!strcmp(name, "vkGetInstanceProcAddr")) return (PFN_vkVoidFunction) layer_GetInstanceProcAddr;
  if (!strcmp(name, "vkGetDeviceProcAddr")) return (PFN_vkVoidFunction) layer_GetDeviceProcAddr;
  PFN_vkVoidFunction f = intercept_instance(name);
  if (f) return f;
  if (!strcmp(name, "vkDestroyDevice")) return (PFN_vkVoidFunction) layer_DestroyDevice;
  f = process_allowed() ? intercept_device(name) : NULL;
  if (f) return f;
  if (!inst) return NULL;
  pthread_mutex_lock(&lock);
  inst_data *d = find_inst(key_of(inst));
  PFN_vkGetInstanceProcAddr gipa = d ? d->gipa : NULL;
  pthread_mutex_unlock(&lock);
  return gipa ? gipa(inst, name) : NULL;
}

EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface *v) {
  if (v->loaderLayerInterfaceVersion > 2) v->loaderLayerInterfaceVersion = 2;
  v->pfnGetInstanceProcAddr = layer_GetInstanceProcAddr;
  v->pfnGetDeviceProcAddr = layer_GetDeviceProcAddr;
  v->pfnGetPhysicalDeviceProcAddr = NULL;
  return VK_SUCCESS;
}
