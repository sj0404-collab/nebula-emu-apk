using System;
using System.Linq;
class T { static void Main() { Console.WriteLine("compiled inside the emulator: " + string.Join("+", Enumerable.Range(1, 5)) + " = " + Enumerable.Range(1, 5).Sum()); } }
