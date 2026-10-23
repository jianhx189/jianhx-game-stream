@echo off
whoami /groups > "D:\远程项目\tools\elev_probe.txt" 2>&1
echo ---MARKER--- >> "D:\远程项目\tools\elev_probe.txt"
net session >> "D:\远程项目\tools\elev_probe.txt" 2>&1
