# Egg-programming-language
A programming language named egg/VEX with a compiler called gutterball (file type is .egg)


it only runs on Linux machines, so to start using it you do this:

1. Download source code
2. Extract it
3. go to the terminal and do
   
             cd ~/Downloads/Egg-programming-language-main
   
4. Set it up
   
               chmod +x install.sh gutterball
               ./install.sh
   
5. Make your first program:

             nano hello.egg
   
6. Write in the code:

                fn main() -> void {
                   execute.WriteIn("Hello World!")
                   execute.WriteInt(10 + 5)
                   execute.Exit(0)
                }
    
7. Compile:

              gutterball hello.egg -o hello
9. Run:

              ./hello

Now you can start programming in egg/VEX!
    
               
