/////////////////////////////////////////////////////////////////
// HC908JB8 USB ICP Manager v1.0 - (c) Juha Forsten, 5 Jun 2007 
// =============================================================
//
// USB InCircuitProgramming Tool for 
// Freescale (Motorola) HC908 JB8 microcontroller 
// (Based on the firmware from Freescale ApplicationNote AN2398)
//
// Requirements: 
// * ICP Resident code programmed to the device
// * User Code support for ICP Flag clear with 
//   "SetFeature" USB (HID) Command 
//
//
// Compile:
// ======== 
// MinGW: gcc manage.c -o manage -lusb -O2 -Wall
//
//
// Version Log:  
// =============
//  1.0:
//
//    - First release
//
/////////////////////////////////////////////////////////////////


#include <usb.h>
#include <stdio.h>
#include <unistd.h>

#ifdef __MINGW32__
// WINDOWS

// For Sleep()
#include <windows.h>

#else
// UNIX

void Sleep(unsigned int  ms )
{
    // 1 milliseconds = 1000 microsecond.
    // Windows Sleep uses miliseconds
    // linux usleep uses microsecond
    // and since the default
    // usage of the code was under windows 
    // so the argument is coming in millisecond.
    usleep( ms * 1000 );
}
#endif

#define MAX_FILENAME_SIZE 255

/* the device's vendor and product id */
// HID Mode
#define HID_VID 0x0c74
#define HID_PID 0x4008
// ICP Mode
#define ICP_VID 0x0425
#define ICP_PID 0xff01

#define MAX_LINE_LEN 512
#define BUF_SIZE 0x40

#define MEM_SIZE 0x1C00           //7168 bytes
#define MEM_OFFSET 0xDC00
#define MEM_BLOCK_SIZE 0x200      // 512 bytes
#define MEM_PROG_BLOCK_SIZE 0x40  // 64 bytes

#define WAIT_PROGRAMMING 70       // Wait after sent ICP programming command to device
#define WAIT_STATUS       5       // Wait after sent ICP satus commnd to device

#define ICP_CHECKSUM_START 0xF600
#define ICP_CHECKSUM_STOP  0XF7FD
#define ICP_FLAG_ADDRESS   0xF7FE

#define HRM_OK 1
#define HRM_ERROR -1


// Conditional printf: if x==1, second argument is printed using standard printf
//
// NOTE: Uses "variadic macro"-definition. See: http://en.wikipedia.org/wiki/Variadic_macro
#define HRM_printf(x,...) if(x) {printf(__VA_ARGS__);fflush(stdout);}

// Errorcodes and errormessager /////////////////////////////////////////////////////////////
#define HRM_NO_ERRORS           0 
#define HRM_USB_OPEN_ERROR      1
#define HRM_USB_CONFIG_ERROR    2
#define HRM_FILE_OPEN_ERROR     3
#define HRM_FLASH_ERASE_ERROR   4
#define HRM_FLASH_PROGRAM_ERROR 5

static char *HRM_Errors[]=
{
  "No Errors",                               // 0
  "USB Device not found!\n",                 // 1
  "Settinbg USB configuration failed!\n",    // 2
  "File not found\n",                        // 3
  "Flash Erase failed!\n",                   // 4
  "Flash Program failed!\n",                 // 5
};

// HRM Datatype ///////////////////////////////////////////////////////////////////////////////
typedef struct HRM_Data {

  char *filename;                    // Filename of the S19-file

  unsigned int icp_flag_calculated;  // ICP-Flag based on the data
  unsigned int icp_flag;             // ICP-Flag from file

  usb_dev_handle *usb_dev;            // USB Handle

  unsigned char mem[MEM_SIZE];        // Data to program to device

  unsigned char verbose_mode;      // If >0, functions prints info
  unsigned char last_errorcode;   // If error occured, errorcode is saved here

} HRM_Data;


/////////////////////////////////////////////////////////////////////////////////////
// HRM_CheckError                                                                  //
// ==============                                                                  //
// - Check, if last function caused an error, prints info and exits                //
/////////////////////////////////////////////////////////////////////////////////////
void HRM_CheckError(HRM_Data *hrm)
{
  if(hrm->last_errorcode > 0) {
    fprintf(stderr,"ERROR: %s\n",HRM_Errors[hrm->last_errorcode]);
    exit(hrm->last_errorcode);
  }
}

/////////////////////////////////////////////////////////////////////////////////////
// HRM_OpenUSB                                                                     //
// ===========                                                                     //
// - Opens USB connection (LibUSB)                                                 //
/////////////////////////////////////////////////////////////////////////////////////
usb_dev_handle *HRM_OpenUSB(unsigned int vid, unsigned int pid)
{
  struct usb_bus *bus;
  struct usb_device *dev;

  // LibUSB functions
  usb_init(); /* initialize the library */
  usb_find_busses(); /* find all busses */
  usb_find_devices(); /* find all connected devices */


  //LibUSB functions
  for(bus = usb_get_busses(); bus; bus = bus->next) 
    {
      for(dev = bus->devices; dev; dev = dev->next) 
        {
          if(dev->descriptor.idVendor == vid
             && dev->descriptor.idProduct == pid)
            {
              return usb_open(dev);
            }
        }
    }
  return NULL;
}

////////////////////////////////////////////////////////////////////////////////////
// HRM_CloseUSB                                                                   //
// ================                                                               //
// - Closes the USB Connection (LibUSB) (name mapping only!)                      //
////////////////////////////////////////////////////////////////////////////////////
#define HRM_CloseUSB usb_close


////////////////////////////////////////////////////////////////////////////////////
// HRM_ICP_InitUSB                                                                //
// ===============                                                                //
// - Initializes and opens the USB connection for ICP                             //
////////////////////////////////////////////////////////////////////////////////////
int HRM_ICP_InitUSB(HRM_Data *hrm)
{
  // LibUSB functions
  usb_init(); /* initialize the library */
  usb_find_busses(); /* find all busses */
  usb_find_devices(); /* find all connected devices */

  // Reset Errors
  hrm->last_errorcode=0;

  if(!(hrm->usb_dev = HRM_OpenUSB(ICP_VID,ICP_PID)))
    {
      // Device Not Found!
      hrm->last_errorcode=HRM_USB_OPEN_ERROR;
      return(HRM_ERROR);
    }

  if(usb_set_configuration(hrm->usb_dev, 1) < 0)
    {
      // Setting configuration failed!
      hrm->last_errorcode=HRM_USB_CONFIG_ERROR;
      usb_close(hrm->usb_dev);
      return(HRM_ERROR);
    }
  return(HRM_OK);
}

////////////////////////////////////////////////////////////////////////////////////
// HRM_ICP_CloseUSB                                                               //
// ================                                                               //
// - Closes the USB connection for ICP                                            //
////////////////////////////////////////////////////////////////////////////////////
void HRM_ICP_CloseUSB(HRM_Data *hrm)
{
  //LibUSB function
  usb_close(hrm->usb_dev);
}

////////////////////////////////////////////////////////////////////////////////////
// HRM_ICP_ReadS19                                                                //
// ===============                                                                //
// - Read and Parse S19-file to HRM_Data struct                                   //
////////////////////////////////////////////////////////////////////////////////////
int HRM_ICP_ReadS19(HRM_Data *hrm)
{
  unsigned char crc,calc_crc,calc_datalen,sdata;
  int i,readlen,address,datalen;
  char line[MAX_LINE_LEN],address_str[5],datalen_str[3],sdata_str[3];
  FILE *fp;

  // Reset Errors
  hrm->last_errorcode=0;

  // Init strings
  address_str[4]=0;
  sdata_str[2]=0;
  datalen_str[2]=0;

  // Init memory to 0xFF (=default empty flash)
  for(i=0;i<MEM_SIZE;i++)
    hrm->mem[i]=0xff;

  // Open file
  if( (fp = fopen(hrm->filename,"r")) == NULL) {
    //File not found!");
    hrm->last_errorcode=HRM_FILE_OPEN_ERROR;
    return(HRM_ERROR);
  }

  // Read line by line and do the parsing...
  while(!feof(fp)) {

    if(fgets(line,MAX_LINE_LEN,fp) == NULL) {
      //printf("Nothing to read!");
      break;
    }

    //Search S1-entry.. and parse when found.
    if(line[0]=='S' && line[1]=='1') {

      calc_crc=0;

      // Datalen
      readlen=strlen(line);
      calc_datalen=(readlen-5)/2;
      strncpy(datalen_str,line+2,2);
      datalen= strtoul(datalen_str,NULL,16);

      // Address
      strncpy(address_str,line+4,4);
      address= strtoul(address_str,NULL,16);

      // Check address range
      if((address < MEM_OFFSET) || (address >= MEM_OFFSET+MEM_SIZE )) {
	continue;
      }

      // Data
      for(i=0;i<datalen-3;i++) {
	// Get data byte
	strncpy(sdata_str,line+8+2*i,2);
	sdata=strtoul(sdata_str,NULL,16);
	// Put to memmap
	hrm->mem[address-MEM_OFFSET+i]=sdata;
	// incr crc
	calc_crc+=sdata;
      }

      // CRC
      strncpy(sdata_str,line+8+2*i,2);
      crc=strtoul(sdata_str,NULL,16);
      calc_crc=0xff-(calc_crc + datalen + (address>>8) + (address & 0xff));

    } else if(line[0]=='S' && line[1]=='9') {
      // EndOfRecord
      readlen=strlen(line);
      break;

    } else {
      // Not Supported
    }
  }

  //ICP Flag (Checksum)
  hrm->icp_flag_calculated=0;
  for(i=ICP_CHECKSUM_START;i<=ICP_CHECKSUM_STOP;i++) {
    hrm->icp_flag_calculated+=hrm->mem[i-MEM_OFFSET];
  }

  hrm->icp_flag_calculated=0xffff-(0xffff & hrm->icp_flag_calculated)+1;
  hrm->icp_flag=(hrm->mem[ICP_FLAG_ADDRESS-MEM_OFFSET]<<8) + hrm->mem[ICP_FLAG_ADDRESS-MEM_OFFSET+1];
  
  return(HRM_OK);
}

////////////////////////////////////////////////////////////////////////////////////
// HRM_ICP_EraseFlashBlock                                                        //
// =======================                                                        //
// - Erase one block of Flash memory (size of 512)                                //
////////////////////////////////////////////////////////////////////////////////////
int HRM_ICP_EraseFlashBlock(HRM_Data *hrm, unsigned int block_start_addr)
{
  unsigned char status,result;

  // Reset Errors
  hrm->last_errorcode=0;

  // Validity check
  if(hrm->usb_dev == NULL) {
    hrm->last_errorcode=HRM_FLASH_ERASE_ERROR;
    return(HRM_ERROR);
  }

  // ERASE BLOCK
  result = usb_control_msg(
			   hrm->usb_dev,       // USB-Device
			   0x40,
			   0x82,
			   block_start_addr,
			   block_start_addr+MEM_BLOCK_SIZE-1,
			   NULL,
			   0x00,
			   10000);
  
  // GET RESULT

  Sleep(5);
  result = usb_control_msg(
			   hrm->usb_dev,       // USB-Device
			   0xC0,
			   0x8F,
			   0x0000,
			   0x0000,
			   &status,
			   0x01,
			   10000);
  Sleep(5);    

  // Check error
  if( (status != 1) || (result != 1)) {
    hrm->last_errorcode=HRM_FLASH_ERASE_ERROR;
    return(HRM_ERROR);
  }
  return(HRM_OK);
}

////////////////////////////////////////////////////////////////////////////////////
// HRM_ICP_EraseFlash                                                             //
// ==================                                                             //
// - Erase whole user area of the Flash memory (0xDC00-0xF7FF)                    //
////////////////////////////////////////////////////////////////////////////////////
int HRM_ICP_EraseFlash(HRM_Data *hrm)
{
  int i;

  // Reset Errors
  hrm->last_errorcode=0;

  // ERASE ALL BLOCKS
  HRM_printf(hrm->verbose_mode,"\nERASING FLASH:\n======================\n");

  for(i=MEM_OFFSET; i<MEM_OFFSET+MEM_SIZE; i=i+MEM_BLOCK_SIZE) {

    HRM_printf(hrm->verbose_mode,"\n0x%04X: ",i);

    if (HRM_ICP_EraseFlashBlock(hrm, i) == HRM_ERROR) {
      hrm->last_errorcode=HRM_FLASH_ERASE_ERROR;
      return(HRM_ERROR);
    }

    HRM_printf(hrm->verbose_mode,"EEEEEEEE");
  }

  HRM_printf(hrm->verbose_mode,"\n");
  return(HRM_OK);
}

////////////////////////////////////////////////////////////////////////////////////
// HRM_ICP_ProgramFlash                                                           //
// =====================                                                          //
// - Program whole user area of the Flash memory (0xDC00-0xF7FF)                  //
////////////////////////////////////////////////////////////////////////////////////
int HRM_ICP_ProgramFlash(HRM_Data *hrm)
{
  unsigned char status,result,valid;
  int i,j,l=0;
  
  // Reset Errors
  hrm->last_errorcode=0;

  HRM_printf(hrm->verbose_mode,"\nPROGRAMMING FLASH:\n======================\n");

  // PROGRAM ALL BLOCKS
  for(i=MEM_OFFSET;i<MEM_OFFSET+MEM_SIZE;i=i+MEM_PROG_BLOCK_SIZE) {

    // Print some progress info
    if((l%8) == 0) {
      HRM_printf(hrm->verbose_mode,"\n0x%04X: ",i);
    }

    // Check, if block has something else than "0xff"..
    valid=0;
    for(j=0;j<MEM_PROG_BLOCK_SIZE;j++) {
      if(hrm->mem[i-MEM_OFFSET+j] != 0xff) {
	valid=1;
	break;
      }
    }
    
    if(valid) {
      // PROGRAM BLOCK
      result = usb_control_msg(
			       hrm->usb_dev,       // USB-Device
			       0x40,
			       0x81,
			       i,
			       i+MEM_PROG_BLOCK_SIZE-1,
			       hrm->mem + i-MEM_OFFSET,
			       MEM_PROG_BLOCK_SIZE,
			       10000);

      // Check error..
      if( (result != MEM_PROG_BLOCK_SIZE)) {
	hrm->last_errorcode=HRM_FLASH_PROGRAM_ERROR;
	return(HRM_ERROR);
      }      

      Sleep(WAIT_PROGRAMMING);
      
      // GET RESULT
      result = usb_control_msg(
			       hrm->usb_dev,       // USB-Device
			       0xC0,
			       0x8F,
			       0x0000,
			       0x0000,
			       &status,
			       0x01,
			       10000);

      // Check error..
      if( (result != 1) || (result != 1)) {
	hrm->last_errorcode=HRM_FLASH_PROGRAM_ERROR;
	return(HRM_ERROR);
      }      

      Sleep(WAIT_STATUS);    

      HRM_printf(hrm->verbose_mode,"P");

    } else {
      
      HRM_printf(hrm->verbose_mode,".");

    }
    l++;
  }
  
  HRM_printf(hrm->verbose_mode,"\n");
  
  return(HRM_OK);
}

////////////////////////////////////////////////////////////////////////////////////
// HRM_ClearICPFlag                                                               //
// =====================                                                          //
// - Clear ICP Flag when in HID mode.                                             //
////////////////////////////////////////////////////////////////////////////////////
int HRM_ClearICPFlag(unsigned int vid, unsigned int pid,unsigned int key1, unsigned int key2) 
{
  usb_dev_handle *dev = NULL; /* the device handle */
  unsigned char result;
  unsigned char tmp[8];

  // Open USB
  if( (dev=HRM_OpenUSB(vid,pid)) == NULL) {
    return(HRM_ERROR);
  }

  // Set configuration
  if(usb_set_configuration(dev, 1) < 0)
    {
      usb_close(dev);
      return(HRM_ERROR);
    }

  usb_clear_halt(dev,0);
  usb_resetep(dev,0);

  // Send Set Feature command with keys
  result = usb_control_msg(
			   dev,       // USB-Device
			   0x21,
			   0x09,
			   key1,
			   key2,
			   tmp,
			   8,
			   10000);
  HRM_CloseUSB(dev);
  return 0;  
}



//// MAIN ////////////////////////
/////////////////////////////////
////////////////////////////////

int main(int argc, char **argv)
{
  HRM_Data hrm;
  unsigned int i,key1,key2;

  printf("\n");
  printf("======================\n");
  printf("HRM Flashing Tool v1.0\n");
  printf("======================\n");

  // Check, if Keys are entered as an argumet
  if(argc >= 4) {
    key1=strtoul(argv[2],NULL,16);
    key2=strtoul(argv[3],NULL,16);
    
    printf("\nCLEARING ICP-FLAG:\n");
    printf("======================\n");
    
    printf("Using keys: 0x%04X, 0x%04X \n",key1,key2);
    fflush(stdout);    
    
    if ( HRM_ClearICPFlag(HID_VID, HID_PID, key1, key2) == HRM_ERROR) {
      printf("ERROR: Can't Clear ICP Flag!\n");
      exit(HRM_ERROR);
    }
    
    printf("\nICP_Flag cleared!\n\n");
    fflush(stdout);
    //getc(stdin);
  }

  // Initialize USB.. and wait 30 seconds (1 retry/s) for power cycle... 
  for(i=30; i>0; i--) {
    if(HRM_ICP_InitUSB(&hrm) == HRM_OK) {
    	break;
    }
    printf("\r>>> Unplug and Replug the device in %d seconds... <<<",i);
    fflush(stdout);
    Sleep(1000);
  }
  HRM_CheckError(&hrm);
  printf("\r                                                             ");

  // Set verbose mode to 1, ie. have some nice output from functions to screen..
  hrm.verbose_mode = 1;

  // Setup Filename for S19-file
  hrm.filename = argv[1];

  // Read & parse data from S19-file
  printf("\nCHECKING FILE:\n");
  printf("======================\n");
  printf("\"%s\"...",hrm.filename);
  HRM_ICP_ReadS19(&hrm);
  HRM_CheckError(&hrm);
  printf("OK!\n");

  printf("\n");
  printf("ICP FLAGS:\n");
  printf("======================\n");
  printf("From file : 0x%04X\n",hrm.icp_flag);
  printf("Calculated: 0x%04X\n",hrm.icp_flag_calculated);  

  if(hrm.icp_flag != hrm.icp_flag_calculated) {
    printf("\nNOTE: Fixing ICP Flag value automatically!\n");
    printf("ICP FLAG - OLD: %02X%02X ->", 
	   hrm.mem[ICP_FLAG_ADDRESS-MEM_OFFSET],
	   hrm.mem[ICP_FLAG_ADDRESS-MEM_OFFSET+1]);

    hrm.mem[ICP_FLAG_ADDRESS-MEM_OFFSET]= hrm.icp_flag_calculated >> 8;
    hrm.mem[ICP_FLAG_ADDRESS-MEM_OFFSET+1]= hrm.icp_flag_calculated & 0xff;

    printf("NEW: %02X%02X\n", 
	   hrm.mem[ICP_FLAG_ADDRESS-MEM_OFFSET],
	   hrm.mem[ICP_FLAG_ADDRESS-MEM_OFFSET+1]);

  }
  fflush(stdout);
  
  // ERASE ALL BLOCKS  
  HRM_ICP_EraseFlash(&hrm);
  HRM_CheckError(&hrm);

  // PROGRAM FLASH
  HRM_ICP_ProgramFlash(&hrm);
  HRM_CheckError(&hrm);

  // CLOSE
  HRM_ICP_CloseUSB(&hrm);
  
  exit(0);
}
