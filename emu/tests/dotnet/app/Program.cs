using System;
using System.Linq;
using System.Collections.Generic;
using System.Text.Json;
using System.Threading.Tasks;
class P {
  static int Main(string[] a) {
    string mode = a.Length > 0 ? a[0] : "gc";
    Console.WriteLine("mode " + mode);
    if (mode.Contains("async")) return Run(mode).GetAwaiter().GetResult();
    if (mode.Contains("thread")) { int r = 0; var t = new System.Threading.Thread(() => { r = Steps(mode); }); t.Start(); t.Join(); return r; }
    if (mode.Contains("taskrun")) return Task.Run(() => Steps(mode)).GetAwaiter().GetResult();
    if (mode.Contains("bgsleep")) { var t = new System.Threading.Thread(() => { System.Threading.Thread.Sleep(3000); }); t.IsBackground = true; t.Start(); System.Threading.Thread.Sleep(200); return Steps(mode); }
    if (mode.Contains("bgspin")) { var t = new System.Threading.Thread(() => { long x = 0; while (true) x++; }); t.IsBackground = true; t.Start(); System.Threading.Thread.Sleep(200); return Steps(mode); }
    return Steps(mode);
  }
  static async Task<int> Run(string mode) { await Task.Yield(); int r = Steps(mode); await Task.Delay(10); return r; }
  static int Steps(string mode) {
    if (mode.Contains("alloc")) { var junk = new List<byte[]>(); for (int i = 0; i < 2000; i++) junk.Add(new byte[4096]); junk.Clear(); }
    if (mode.Contains("linq")) { var primes = Enumerable.Range(2, 2000).Where(n => Enumerable.Range(2, (int)Math.Sqrt(n) - 1).All(d => n % d != 0)).ToList(); Console.WriteLine("primes " + primes.Count); }
    if (mode.Contains("nre")) { try { object o = null; o.ToString(); } catch (NullReferenceException) { Console.WriteLine("caught NRE"); } }
    if (mode.Contains("throw")) { try { throw new InvalidOperationException("boom"); } catch (InvalidOperationException e) { Console.WriteLine("caught " + e.Message); } }
    if (mode.Contains("culture")) { var ru = new System.Globalization.CultureInfo("ru-RU"); Console.WriteLine("culture " + ru.DisplayName + " | " + new DateTime(2026, 11, 1).ToString("D", ru) + " | " + (1234567.891).ToString("N2", ru) + " | " + "straße".ToUpper(ru) + " | " + string.Compare("a", "B", ru, System.Globalization.CompareOptions.IgnoreCase) + " | " + System.Globalization.CultureInfo.CurrentCulture.Name + " | tz=" + TimeZoneInfo.Local.Id); }
    if (mode.Contains("regex")) { var m = System.Text.RegularExpressions.Regex.Matches("nebula 12 gaming 345 x64", @"\d+"); Console.WriteLine("regex " + string.Join(",", m.Select(x => x.Value))); }
    if (mode.Contains("http")) { try { using var hc = new System.Net.Http.HttpClient(); var r = hc.GetStringAsync("http://example.com/").GetAwaiter().GetResult(); Console.WriteLine("http " + r.Length + " bytes"); } catch (Exception e) { Console.WriteLine("http fail " + e.GetType().Name + ": " + e.Message + " <- " + e.InnerException?.GetType().Name + ": " + e.InnerException?.Message + " <- " + e.InnerException?.InnerException?.Message); } }
    if (mode.Contains("proc")) { try { var pi = new System.Diagnostics.ProcessStartInfo("cmd.exe", "/c echo child-ok") { RedirectStandardOutput = true, UseShellExecute = false }; using var pr = System.Diagnostics.Process.Start(pi)!; Console.WriteLine("proc " + pr.StandardOutput.ReadToEnd().Trim()); pr.WaitForExit(); } catch (Exception e) { Console.WriteLine("proc fail " + e.GetType().Name + ": " + e.Message); } }
    if (mode.Contains("reflect")) { var t = typeof(List<int>); var inst = (System.Collections.IList)Activator.CreateInstance(t)!; inst.Add(7); var dyn = (dynamic)5; Console.WriteLine("reflect " + t.Name + " " + inst.Count + " " + (dyn + 1) + " " + string.Join(",", typeof(P).GetMethods(System.Reflection.BindingFlags.Static | System.Reflection.BindingFlags.NonPublic | System.Reflection.BindingFlags.Public).Select(m => m.Name).OrderBy(n => n).Take(3))); }
    if (mode.Contains("json")) { var json = JsonSerializer.Serialize(new { name = "nebula", items = new[] { 2, 3, 5 }, ok = true }); Console.WriteLine("json " + json); var back = JsonSerializer.Deserialize<Dictionary<string, JsonElement>>(json)!; Console.WriteLine("back " + back["name"].GetString()); }
    if (mode.Contains("gc")) { GC.Collect(); Console.WriteLine("gc ok gen0=" + GC.CollectionCount(0)); }
    return 3;
  }
}
