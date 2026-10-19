using System;
using System.Linq;
namespace BloodborneLauncher.Core;
public static class BindingExpressions {
 public static bool IsChord(string s)=>s.Contains(" + ",StringComparison.Ordinal);
 public static string[] Atoms(string s)=>s.Split(" + ",StringSplitOptions.RemoveEmptyEntries|StringSplitOptions.TrimEntries);
 public static string CaptureToken(string s)=>s switch{","=>"Comma","Keypad ,"=>"Keypad Comma","Keypad +"=>"Keypad Plus",_=>s};
 public static string? Normalize(string s,Func<string,string?> atom){
  var a=Atoms(s).Select(atom).ToArray();if(a.Length==0||a.Any(x=>x==null))return null;
  int Rank(string x)=>x.Contains("Shift")||x.Contains("Ctrl")||x.Contains("Alt")||x.Contains("GUI")?0:InputMap.IsMouse(x)?2:1;
  return string.Join(" + ",a.Select(x=>x!).Distinct(StringComparer.OrdinalIgnoreCase).OrderBy(Rank).ThenBy(x=>x,StringComparer.OrdinalIgnoreCase));
 }
}
