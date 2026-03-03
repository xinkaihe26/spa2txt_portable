# spa2txt_portable

spa2txt app is based on C++ code from: https://github.com/aitap/spa2txt#

is used to convert FTIR .spa file (from OMNIC) to txt for further usage.

source code is based on Visual studio in the folder of spa2txx-master

or you can use the spa2txt.exe directly:

1. copy paste spa2txt.exe to your folder together with your FTIR spectra

2. in Powershell, cd to your directory

3. convert al spa files using command:  Get-ChildItem -Filter *.spa | ForEach-Object { .\spa2txt.exe $_.FullName }


or you can directly use powershell to copy paste .exe in your folder:
Copy-Item "C:\Users\......\spa2txt_portable\spa2txt.exe" .\ -Force
Get-ChildItem -Filter *.spa | ForEach-Object { .\spa2txt.exe $_.FullName }
