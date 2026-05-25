// hello.ts — ToxenOS .ts scripting demo
var name = "ToxenOS"
print("Hello from " + name + "!")

// counting loop
for (var i = 1; i <= 5; i++) {
    print("  count: " + i)
}

// conditional
var x = 10
if (x > 5) {
    print("x is greater than 5")
} else {
    print("x is 5 or less")
}

// list .elf files in SystemT
print("Binaries in SystemT:")
foreach (var f in /C:/BSM/SystemT/*.elf) {
    print("  " + f)
}
