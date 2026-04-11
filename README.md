# Egg-programming-language
A programming language named egg/VEX with a compiler called gutterball (file type is .egg)


it only runs on Linux machines, so to start using it you do this:

1. Extract it on desktop git, or use the terminal to do:

            git clone https://github.com/raulcode-sys/Egg-programming-language
     
2. Then extract it and go to the terminal and do
   
             cd ~/Downloads/Egg-programming-language-main
   
3. Set it up
   
               chmod +x install.sh gutterball
               ./install.sh
   
4. Make your first program:

             nano hello.egg
   
5. Write in the code:

                fn main() -> void {
                   execute.WriteIn("Hello World!")
                   execute.WriteInt(10 + 5)
                   execute.Exit(0)
                }
    
6. Compile:

              gutterball hello.egg -o hello
7. Run:

              ./hello

   NEW FEATURES: Sort Library (Bubble Sort), Low-Level capabilities, Multi-line comments

Now you can start programming in egg/VEX!
    
               
