# Conqueror
<h2>A low level Operating System designed using Linux Kernel</h2>
<p>To develop the basic low level operating system, we need following</p>
<ul>
  <li>Virtual Machine</li>
  <li>Clean Debian OS</li>
  <li>KDE Desktop Environment</li>
</ul>
<p>You need to have basic knowledge on</p>
<ul>
  <li>Programming in C</li>
  <li>x86_64 Assembly Programming</li>
  <li>Linux Basic Commands</li>
</ul>
<p>After getting all this, lets start building the Operating System using <b>Kernel</b></p>
<h2>[$Checking and Creating Hard drive Partitions:~#]</h2>
<ul>
  <li>Open the <b>Terminal</b></li>
  <li>Type <b>lsblk</b> command</li>
  <li>It will display sdb. Its actually a Hard Drive</li>
  <li>Change the user to <b>root</b> by typing the command <b>sudo su</b></li>
  <li>Type <b>cfdisk /dev/sdb</b></li>
  <li>It will open <b>Hard drive partition table</b></li>
  <li>Click on <b>[ New ]</b> and hit enter</li>
  <li>Hit Enter again so that it will allocate default size</li>
  <li>Click on <b>[ Primary ]</b> and hit enter</li>
  <li>Click on <b>[ Bootable ]</b> and hit enter</li>
  <li>And then click on <b>[ Write ]</b> and type <b>yes</b> and hit enter</li>
  <li>Finally, click on <b>[ Quit ]</b> to exit</li>
  <li>Run <b>lsblk</b> command again, then we can see that new partition has been created</li>
  <li>This is how we can create a new <b>Bootable</b> hard drive partition to test our OS</li>
</ul>
<h2>[$Getting Things Ready for Operating System:~#]</h2>
<ul>
  <li>Type <b>mkfs.ext4 /dev/sdb1</b></li>
  <li>Go to the folder /mnt by typing <b>cd /mnt</b></li>
  <li>Create a New Folder by typing <b>mkdir folder_name</b>. That will be Name of your OS.</li>
  <li>Mount Your Folder using <b>mount /dev/sdb1 folder_name</b></li>
  <li>Now, get into the OS by typing <b>cd folder_name</b></li>
  <li>Remove all the things inside the folder by typing <b>rm -rvf *</b></li>
  <li>Create following folders that will be used for OS Creation by typing the command given below</li>
  <li><b>mkdir -p ./{etc,lib,lib64,boot,bin,sbin,var,dev,proc,sys,run,tmp,src}</b></li>
  <li>After creating the folders, set mknod by typing following commands</li>
  <li><b>mknod -m 600 ./dev/console c 5 1</b></br><b>mknod -m 666 ./dev/null c 1 3</b></li>
  <li>Go to boot folder by typing <b>cd boot</b></li>
  <li>Now, let's copy the <b>Kernel File</b> and <b>Initial Ram Disk</b> from <b>/boot</b> folder</li>
  <li><b>cp /bootvmlinuz-4.19.0-10-amd64 .</br>cp /boot/initrd.img-4.19.0-10.amd64</b></li>
  <li>Now, let's install some dependences using following command</li>
  <li><b>grub-install /dev/sdb --skip-fs-probe --boot-directory=/mnt/folder_name/boot</b></li>
  <li>It will install <b>grub</b> folder. Now, let's start coding part</li>
</ul>
<h2>[$Starting Coding Part for Operating System:~#]</h2>
<ul>
  <li>Make sure you are in <b>grub</b> directory created previously</li>
  <li>Create a new file <b>grub.cfg</b> using any editor. My suggestion is to go with <b>VI</b></li>
  <li>The code is at <a href="https://github.com/mahendragandham/Conqueror/blob/main/boot/grub/grub.cfg"><b>[ grub.cfg ]</b></a>. For Explanation, go to <a href="https://github.com/mahendragandham/Conqueror/blob/main/Documentation/grub/readme.md"><b>[ Documentation/grub/readme.md ]</b></a></li>
  <li>After that, Go to src folder created in <b>folder_name</b> or simple type <b>cd ../../src</b></li>
  <li>Create folders <a href="https://github.com/mahendragandham/Conqueror/tree/main/src/lib"><b>[ lib ]</b></a> and <a href="https://github.com/mahendragandham/Conqueror/tree/main/src/init"><b>[ init ]</b></a> using <b>mkdir -p ./{lib,init}</b></li>
  <li>Go to lib folder and create <b><a href="https://github.com/mahendragandham/Conqueror/blob/main/src/lib/start.S"><b>[ start.S ]</b></a></b></li> 
  <li>Get back to <b>src</b> folder and go to init folder and create <b><a href="https://github.com/mahendragandham/Conqueror/blob/main/src/init/init.c">[ init.c ]</a></b></li>
</ul>
