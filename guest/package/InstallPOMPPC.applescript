-- SPDX-License-Identifier: GPL-3.0-or-later
-- Copyright (c) 2026 VERHILLE Arnaud
-- Install POMPPC — l'installateur à double-cliquer du CD POMPPC_GUEST : install.sh avec le mot de
-- passe demandé par Mac OS X, sans Terminal. Compilé dans l'invité par osacompile (job prebuilt).
set appPath to POSIX path of (path to me)
if appPath ends with "/" then set appPath to text 1 thru -2 of appPath
set cdPath to do shell script "dirname " & quoted form of appPath

display dialog "This installs the POMPPC 3D driver, so that OpenGL games run on your Mac's graphics card. Mac OS X will ask for your password." buttons {"Cancel", "Install"} default button "Install" with title "Install POMPPC"

try
	do shell script "sh " & quoted form of (cdPath & "/install.sh") with administrator privileges
on error errorText number errorNumber
	if errorNumber is not -128 then
		display dialog "The installation failed: " & errorText buttons {"OK"} default button "OK" with icon stop with title "Install POMPPC"
	end if
	return
end try

display dialog "The driver is installed. Restart now to start using it." buttons {"Later", "Restart"} default button "Restart" with title "Install POMPPC"
if button returned of result is "Restart" then
	tell application "Finder" to restart
end if
