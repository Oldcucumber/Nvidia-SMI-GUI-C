@echo off
g++ -std=c++17 -O3 -s -flto -static -Wall -Wextra -Werror -municode -o nvidia-smi-gui.exe main.cpp -lgdi32 -lmsimg32 -ldwmapi -ladvapi32 -mwindows
echo Build complete: nvidia-smi-gui.exe
pause
