using System;
using System.IO;
using System.Linq;
class Program {
  static int Main(string[] args) {
    Console.WriteLine("Hello from a .NET Framework 4.8 exe on " + Environment.Version + " / " + Environment.OSVersion);
    Console.WriteLine("linq: " + string.Join(",", new[] { "a", "b" }.OrderByDescending(s => s)));
    try { throw new InvalidOperationException("fx boom"); } catch (Exception e) { Console.WriteLine("caught: " + e.Message); }
    var p = Path.Combine(Path.GetTempPath(), "fx-test.txt"); File.WriteAllText(p, "written by net48 app"); Console.WriteLine("file: " + File.ReadAllText(p));
    Console.WriteLine("args: " + string.Join(" ", args));
    return 5;
  }
}
