using System.Runtime.InteropServices;
namespace BloodborneLauncher.Core;
// Keep IDs in Vulkan enumeration order, exactly as BB_GPU_ID expects.
public sealed record GpuAdapter(int Index,string Name,string Vendor,long Memory,int Type,uint ApiVersion){public string Label=>$"{Name} ({Memory/(1024d*1024*1024):0.#} GB)";}
public static class GpuAdapters {
 private static IReadOnlyList<GpuAdapter>? cache;
 [StructLayout(LayoutKind.Sequential)] private struct CreateInfo{public uint type;public IntPtr next;public uint flags;public IntPtr app;public uint layers;public IntPtr layerNames;public uint extensions;public IntPtr extensionNames;}
 [DllImport("vulkan-1.dll")]private static extern int vkCreateInstance(ref CreateInfo create,IntPtr allocator,out IntPtr instance);
 [DllImport("vulkan-1.dll")]private static extern void vkDestroyInstance(IntPtr instance,IntPtr allocator);
 [DllImport("vulkan-1.dll")]private static extern int vkEnumeratePhysicalDevices(IntPtr instance,ref uint count,[Out]IntPtr[]? devices);
 [DllImport("vulkan-1.dll")]private static extern void vkGetPhysicalDeviceProperties(IntPtr device,IntPtr properties);
 [DllImport("vulkan-1.dll")]private static extern void vkGetPhysicalDeviceMemoryProperties(IntPtr device,IntPtr properties);
 public static IReadOnlyList<GpuAdapter> All(){
  if(cache!=null)return cache;
  var rows=new List<GpuAdapter>();IntPtr instance=IntPtr.Zero,buffer=IntPtr.Zero;
  try{
   var create=new CreateInfo{type=1};if(vkCreateInstance(ref create,IntPtr.Zero,out instance)!=0)return cache=rows;
   uint count=0;if(vkEnumeratePhysicalDevices(instance,ref count,null)!=0||count==0||count>128)return cache=rows;
   var devices=new IntPtr[count];int result=vkEnumeratePhysicalDevices(instance,ref count,devices);if(result!=0&&result!=5)return cache=rows;
   buffer=Marshal.AllocHGlobal(4096);
   for(int i=0;i<count;i++){
    vkGetPhysicalDeviceProperties(devices[i],buffer);
    uint api=unchecked((uint)Marshal.ReadInt32(buffer,0));
    int vendor=Marshal.ReadInt32(buffer,8),type=Marshal.ReadInt32(buffer,16);
    string name=Marshal.PtrToStringUTF8(IntPtr.Add(buffer,20))??$"Vulkan GPU {i}";
    vkGetPhysicalDeviceMemoryProperties(devices[i],buffer);long memory=0;
    int heaps=Math.Clamp(Marshal.ReadInt32(buffer,260),0,16);
    for(int h=0;h<heaps;h++)if((Marshal.ReadInt32(buffer,264+h*16+8)&1)!=0)memory=Math.Max(memory,Marshal.ReadInt64(buffer,264+h*16));
    rows.Add(new(i,name,vendor==0x10de?"NVIDIA":vendor==0x1002?"AMD":vendor==0x8086?"Intel":"",memory,type,api));
   }
  }catch(DllNotFoundException){}catch(EntryPointNotFoundException){}catch(BadImageFormatException){}
  finally{if(buffer!=IntPtr.Zero)Marshal.FreeHGlobal(buffer);if(instance!=IntPtr.Zero)vkDestroyInstance(instance,IntPtr.Zero);}
  return cache=rows;
 }
 public static int SelectedIndex(IniFile ini){
  var name=ini.Get("gpu_name","");if(name.Length>0)return All().FirstOrDefault(g=>g.Name==name)?.Index??-1;
  return int.TryParse(ini.Get("gpu_id","-1"),out var n)&&n>=0&&All().Any(g=>g.Index==n)?n:-1;
 }
 public static GpuAdapter? Selected(IniFile ini){int n=SelectedIndex(ini);return n>=0?All().FirstOrDefault(g=>g.Index==n):All().OrderByDescending(g=>g.ApiVersion>=((1u<<22)|(3u<<12))).ThenByDescending(g=>g.Type==2).ThenBy(g=>g.Type==4?1:0).ThenByDescending(g=>g.Memory).FirstOrDefault();}
 public static string Summary(IniFile ini)=>Selected(ini)?.Label??"Vulkan device information unavailable";
}
