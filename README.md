# Rootkit

##### This rootkit exploitation assumes root was temporarily obtained through another means and allows a user to return later with permanent root access.
<br>

##### There are two parts to the exploitation:

##### 1. Hiding malicious files 
<br>
<img src="/screenshots/hide_files.png"/>

###### Files prefixed with '$sys$' are visible before the module is inserted but not visible after.
  
  <br>

##### 2. Allowing a non-root user to run programs as root 
<br>
<img src=/screenshots/elevate_non-root.png/>

###### User student is itself when whoami is run before the module is inserted but root after.
