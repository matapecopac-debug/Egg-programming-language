# Egg-programming-language
A programming language named egg/vex with a compiler called gutterball (file type is .egg)


it only runs on Linux machines, so to start using it you do this:

1. Download source code
2. Extract it
3. go to the terminal and do
             cd ~/Downloads/Egg-programming-language-main
4. Make the compiler executable:
             chmod +x gutterball
5. Allow it to run systemwide:
             sudo cp gutterball /usr/local/bin/gutterball
6. Make your first program:
              nano hello.egg
7. Write in the code:
              fn main() -> void {
                execute.WriteIn("Hello from Vex!")
                execute.WriteInt(10 + 5)
                execute.Exit(0)
              }
8. Compile:
              gutterball hello.egg -o hello
9. Run:
              ./hello
               
