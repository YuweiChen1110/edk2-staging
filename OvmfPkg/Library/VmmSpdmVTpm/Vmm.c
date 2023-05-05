/** @file

  Copyright (c) 2022 - 2023, Intel Corporation. All rights reserved. <BR>
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <PiDxe.h>
#include <Library/UefiLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <IndustryStandard/Tdx.h>
#include <Library/TdxLib.h>
#include <Library/MemEncryptTdxLib.h>
#include "VmmSpdmInternal.h"

#define SERVICE_VTPM_SEND_MESSAGE     1
#define SERVICE_VTPM_RECEIVE_MESSAGE  2
#define VMM_SPDM_TIMEOUT              2000        // 2000ms

#define VTPM_DEFAULT_ALLOCATION_PAGE  1
#define VTPM_DEFAULT_MAX_BUFFER_SIZE  0x1000

#pragma pack(1)
// GHCI 2.0 Table 3-45
typedef struct {
  UINT8     Guid[16];
  UINT32    Length;
  UINT32    Reserved;
  // UINT8 Data[];
} TDVMCALL_SERVICE_COMMAND_HEADER;

// GHCI 2.0 Table 3-46
typedef struct {
  UINT8     Guid[16];
  UINT32    Length;
  UINT32    Status;
  // UINT8 Data[];
} TDVMCALL_SERVICE_RESPONSE_HEADER;

// VTPM 0.6.5 Table 5-1
typedef struct {
  UINT8     Version;
  UINT8     Command;
  UINT16    Reserved;
  // UINT8 SecureTpmMessage[];
} SEND_MESSAGE_COMMAND_HEADER;

// VTPM 0.6.5 Table 5-2
typedef struct {
  UINT8    Version;
  UINT8    Command;
  UINT8    Status;
  UINT8    Reserved;
} SEND_MESSAGE_RESPONSE_HEADER;

// VTPM 0.6.5 Table 5-3
typedef struct {
  UINT8     Version;
  UINT8     Command;
  UINT16    Reserved;
} RECEIVE_MESSAGE_COMMAND_HEADER;

// VTPM 0.6.5 Table 5-4
typedef struct {
  UINT8    Version;
  UINT8    Command;
  UINT8    Status;
  UINT8    Reserved;
  // UINT8   SecureTpmMessage[];
} RECEIVE_MESSAGE_RESPONSE_HEADER;

#pragma pack()

TDVMCALL_SERVICE_COMMAND_HEADER  mTdvmcallServiceCommandHeaderTemplate = {
  { 0x93, 0x07, 0x59, 0x64, 0x52, 0x78, 0x52, 0x4e, 0xbe, 0x45, 0xcd, 0xbb, 0x11, 0x6f, 0x20, 0xf3 }, // Guid
  0,                                                                                                  // Length
  0                                                                                                   // Reserved
};

TDVMCALL_SERVICE_COMMAND_HEADER  mTdvmcallServiceResponseHeaderTemplate = {
  { 0x93, 0x07, 0x59, 0x64, 0x52, 0x78, 0x52, 0x4e, 0xbe, 0x45, 0xcd, 0xbb, 0x11, 0x6f, 0x20, 0xf3 }, // Guid
  0,                                                                                                  // Length
  0                                                                                                   // Status
};

SEND_MESSAGE_COMMAND_HEADER  mSendMessageCommandHeaderTemplate = {
  0,
  SERVICE_VTPM_SEND_MESSAGE,
  0
};

SEND_MESSAGE_RESPONSE_HEADER  mSendMessageResponseHeaderTemplate = {
  0,
  SERVICE_VTPM_SEND_MESSAGE,
  0,
  0
};

RECEIVE_MESSAGE_COMMAND_HEADER  mReceiveMessageCommandHeaderTemplate = {
  0,
  SERVICE_VTPM_RECEIVE_MESSAGE,
  0
};

RECEIVE_MESSAGE_RESPONSE_HEADER  mReceiveMessageResponseHeaderTemplate = {
  0,
  SERVICE_VTPM_RECEIVE_MESSAGE,
  0,
  0
};

/**

  This function dump raw data.

  @param  Data  raw data
  @param  Size  raw data size

**/
STATIC
VOID
InternalDumpData (
  IN UINT8  *Data,
  IN UINTN  Size
  )
{
  UINTN  Index;

  for (Index = 0; Index < Size; Index++) {
    DEBUG ((DEBUG_INFO, "%02x ", (UINTN)Data[Index]));
    if (Index == 15) {
      DEBUG ((DEBUG_INFO, "|"));
    }
  }
}

/**

  This function dump raw data with colume format.

  @param  Data  raw data
  @param  Size  raw data size

**/
VOID
VmmSpdmVTpmDumpHex (
  IN UINT8  *Data,
  IN UINTN  Size
  )
{
  UINTN  Index;
  UINTN  Count;
  UINTN  Left;

  #define COLUME_SIZE  (16 * 2)

  Count = Size / COLUME_SIZE;
  Left  = Size % COLUME_SIZE;
  for (Index = 0; Index < Count; Index++) {
    DEBUG ((DEBUG_INFO, "%04x: ", Index * COLUME_SIZE));
    InternalDumpData (Data + Index * COLUME_SIZE, COLUME_SIZE);
    DEBUG ((DEBUG_INFO, "\n"));
  }

  if (Left != 0) {
    DEBUG ((DEBUG_INFO, "%04x: ", Index * COLUME_SIZE));
    InternalDumpData (Data + Index * COLUME_SIZE, Left);
    DEBUG ((DEBUG_INFO, "\n"));
  }
}

STATIC
VOID *
AllocateSharedBuffer (
  UINT32  Pages
  )
{
  EFI_STATUS  Status;
  VOID        *Buffer;

  Buffer = AllocatePages (Pages);
  if (Buffer == NULL) {
    return NULL;
  }

  Status = MemEncryptTdxSetPageSharedBit (0, (PHYSICAL_ADDRESS)Buffer, Pages);
  if (EFI_ERROR (Status)) {
    FreePages (Buffer, Pages);
    Buffer = NULL;
  }

  return Buffer;
}

STATIC
EFI_STATUS
FreeSharedBuffer (
  UINT8   *Buffer,
  UINT32  Pages
  )
{
  EFI_STATUS  Status;

  if ((Buffer == NULL) || (Pages == 0)) {
    return EFI_INVALID_PARAMETER;
  }

  Status = MemEncryptTdxSetPageSharedBit (1, (PHYSICAL_ADDRESS)Buffer, Pages);
  if (!EFI_ERROR (Status)) {
    FreePages (Buffer, Pages);
  }

  return Status;
}

EFI_STATUS
VTpmContextWrite (
  IN UINTN       RequestSize,
  IN CONST VOID  *Request,
  IN UINT64      Timeout
  )
{
  UINT64      RetCode;
  UINT64      RetValue;
  UINT32      DataLen;
  UINT64      TdxSharedBit;
  UINT8       *Ptr;
  UINT8       *CmdBuffer;
  UINT8       *RspBuffer;
  EFI_STATUS  Status;

  TDVMCALL_SERVICE_RESPONSE_HEADER  *TdvmcallRspHeader;
  SEND_MESSAGE_RESPONSE_HEADER      *SendMessageRspHeader;

  // Allocate CmdBuffer and RspBuffer
  CmdBuffer = NULL;
  RspBuffer = NULL;

  CmdBuffer = (UINT8 *)AllocateSharedBuffer (VTPM_DEFAULT_ALLOCATION_PAGE);
  if (CmdBuffer == NULL) {
    Status = EFI_UNSUPPORTED;
    goto QuitVTPMContextWrite;
  }

  RspBuffer = (UINT8 *)AllocateSharedBuffer (VTPM_DEFAULT_ALLOCATION_PAGE);
  if (RspBuffer == NULL) {
    Status = EFI_UNSUPPORTED;
    goto QuitVTPMContextWrite;
  }

  // Build send_message cmd packet
  Ptr = CmdBuffer + sizeof (TDVMCALL_SERVICE_COMMAND_HEADER);
  CopyMem (Ptr, &mSendMessageCommandHeaderTemplate, sizeof (SEND_MESSAGE_COMMAND_HEADER));
  Ptr += sizeof (SEND_MESSAGE_COMMAND_HEADER);
  CopyMem (Ptr, Request, RequestSize);
  Ptr    += RequestSize;
  DataLen = Ptr - CmdBuffer;

  // Build tdvmcall_service cmd packet
  Ptr = CmdBuffer;
  CopyMem (Ptr, &mTdvmcallServiceCommandHeaderTemplate, sizeof (TDVMCALL_SERVICE_COMMAND_HEADER));
  ((TDVMCALL_SERVICE_COMMAND_HEADER *)Ptr)->Length = DataLen;

  // Build send_message rsp packet
  Ptr = RspBuffer + sizeof (TDVMCALL_SERVICE_RESPONSE_HEADER);
  CopyMem (Ptr, &mSendMessageResponseHeaderTemplate, sizeof (SEND_MESSAGE_RESPONSE_HEADER));
  Ptr    += sizeof (SEND_MESSAGE_RESPONSE_HEADER);
  DataLen = Ptr - RspBuffer;

  // Build tdvmcall_service rsp packet
  Ptr = RspBuffer;
  CopyMem (Ptr, &mTdvmcallServiceResponseHeaderTemplate, sizeof (TDVMCALL_SERVICE_RESPONSE_HEADER));
  ((TDVMCALL_SERVICE_RESPONSE_HEADER *)Ptr)->Length = DataLen;

  // Call tdvmcall service to send cmd.
  TdxSharedBit = TdSharedPageMask ();
  if (TdxSharedBit == 0) {
    DEBUG ((DEBUG_ERROR, "%a: Failed with TdxSharedBit %llx\n", __FUNCTION__, TdxSharedBit));
    Status = EFI_ABORTED;
    goto QuitVTPMContextWrite;
  }

  RetCode = TdVmCall (
                      TDVMCALL_SERVICE,
                      (UINT64)CmdBuffer | TdxSharedBit,
                      (UINT64)RspBuffer | TdxSharedBit,
                      0,
                      VMM_SPDM_TIMEOUT,
                      &RetValue
                      );

  if ((RetCode != 0) || (RetValue != 0)) {
    DEBUG ((DEBUG_ERROR, "%a: Failed with RetCode %llx, RetValue %llx\n", __FUNCTION__, RetCode, RetValue));
    Status = EFI_ABORTED;
    goto QuitVTPMContextWrite;
  }

  // Check the status in TDVMCALL_SERVICE
  TdvmcallRspHeader = (TDVMCALL_SERVICE_RESPONSE_HEADER *)RspBuffer;
  if (TdvmcallRspHeader->Status != 0) {
    DEBUG ((DEBUG_ERROR, "%a: Failed with TdvmcallRsp status: %x\n", __FUNCTION__, TdvmcallRspHeader->Status));
    Status = EFI_ABORTED;
    goto QuitVTPMContextWrite;
  }

  // Check the status in SEND_MESSAGE_RESPONSE
  SendMessageRspHeader = (SEND_MESSAGE_RESPONSE_HEADER *)(RspBuffer + sizeof (TDVMCALL_SERVICE_RESPONSE_HEADER));
  if (SendMessageRspHeader->Status != 0) {
    DEBUG ((DEBUG_ERROR, "%a: Failed with SendMessageRsp status: %x\n", __FUNCTION__, SendMessageRspHeader->Status));
    Status = EFI_ABORTED;
    goto QuitVTPMContextWrite;
  }

  Status = EFI_SUCCESS;

QuitVTPMContextWrite:
  if (CmdBuffer) {
    FreeSharedBuffer (CmdBuffer, VTPM_DEFAULT_ALLOCATION_PAGE);
  }

  if (RspBuffer) {
    FreeSharedBuffer (RspBuffer, VTPM_DEFAULT_ALLOCATION_PAGE);
  }

  return Status;
}

/**
  ReadBuffer from vTPM-TD
**/
EFI_STATUS
VTpmContextRead (
  IN OUT UINTN  *ResponseSize,
  IN OUT VOID   *Response,
  IN UINT64     Timeout
  )
{
  UINT64      RetCode;
  UINT64      RetValue;
  UINT8       *Ptr;
  UINT32      DataLen;
  UINT32      HeaderLen;
  UINT64      TdxSharedBit;
  UINT8       *CmdBuffer;
  UINT8       *RspBuffer;
  UINT32      BufferSize;
  EFI_STATUS  Status;

  TDVMCALL_SERVICE_RESPONSE_HEADER  *TdvmcallRspHeader;
  RECEIVE_MESSAGE_RESPONSE_HEADER   *ReceiveMessageRspHeader;

  // Allocate CmdBuffer and RspBuffer
  CmdBuffer  = NULL;
  RspBuffer  = NULL;
  BufferSize = EFI_PAGES_TO_SIZE (VTPM_DEFAULT_ALLOCATION_PAGE);

  CmdBuffer = (UINT8 *)AllocateSharedBuffer (VTPM_DEFAULT_ALLOCATION_PAGE);
  if (CmdBuffer == NULL) {
    Status = EFI_UNSUPPORTED;
    goto QuitVTPMContextRead;
  }

  RspBuffer = (UINT8 *)AllocateSharedBuffer (VTPM_DEFAULT_ALLOCATION_PAGE);
  if (RspBuffer == NULL) {
    Status = EFI_UNSUPPORTED;
    goto QuitVTPMContextRead;
  }

  // Build send_message cmd packet
  Ptr = CmdBuffer + sizeof (TDVMCALL_SERVICE_COMMAND_HEADER);
  CopyMem (Ptr, &mReceiveMessageCommandHeaderTemplate, sizeof (RECEIVE_MESSAGE_COMMAND_HEADER));
  Ptr    += sizeof (RECEIVE_MESSAGE_COMMAND_HEADER);
  DataLen = Ptr - CmdBuffer;

  Ptr = CmdBuffer;
  CopyMem (Ptr, &mTdvmcallServiceCommandHeaderTemplate, sizeof (TDVMCALL_SERVICE_COMMAND_HEADER));
  ((TDVMCALL_SERVICE_COMMAND_HEADER *)Ptr)->Length = DataLen;

  // Build recieve_message rsp packet
  Ptr = RspBuffer + sizeof (TDVMCALL_SERVICE_RESPONSE_HEADER);
  CopyMem (Ptr, &mReceiveMessageResponseHeaderTemplate, sizeof (RECEIVE_MESSAGE_RESPONSE_HEADER));
  Ptr    += sizeof (RECEIVE_MESSAGE_RESPONSE_HEADER);
  DataLen = BufferSize;

  // Build tdvmcall_service rsp packet
  Ptr = RspBuffer;
  CopyMem (Ptr, &mTdvmcallServiceResponseHeaderTemplate, sizeof (TDVMCALL_SERVICE_RESPONSE_HEADER));
  ((TDVMCALL_SERVICE_RESPONSE_HEADER *)Ptr)->Length = DataLen;

  // step c. call tdvmcall service to send command.
  TdxSharedBit = TdSharedPageMask ();
  if (TdxSharedBit == 0) {
    DEBUG ((DEBUG_ERROR, "%a: Failed with TdxSharedBit %llx\n", __FUNCTION__, TdxSharedBit));
    Status = EFI_ABORTED;
    goto QuitVTPMContextRead;
  }

  RetCode = TdVmCall (
                      TDVMCALL_SERVICE,
                      (UINT64)CmdBuffer | TdxSharedBit,
                      (UINT64)RspBuffer | TdxSharedBit,
                      0,
                      VMM_SPDM_TIMEOUT,
                      &RetValue
                      );

  if ((RetCode != 0) || (RetValue != 0)) {
    DEBUG ((DEBUG_ERROR, "%a: Failed with RetCode %llx, RetValue %llx\n", __FUNCTION__, RetCode, RetValue));
    Status = EFI_ABORTED;
    goto QuitVTPMContextRead;
  }

  // Check the status in TDVMCALL_SERVICE
  TdvmcallRspHeader = (TDVMCALL_SERVICE_RESPONSE_HEADER *)RspBuffer;
  if (TdvmcallRspHeader->Status != 0) {
    DEBUG ((DEBUG_ERROR, "%a: Failed with TdvmcallRsp status: %x\n", __FUNCTION__, TdvmcallRspHeader->Status));
    Status = EFI_ABORTED;
    goto QuitVTPMContextRead;
  }

  // Check the status in RECEIVE_MESSAGE_RESPONSE
  ReceiveMessageRspHeader = (RECEIVE_MESSAGE_RESPONSE_HEADER *)(RspBuffer + sizeof (TDVMCALL_SERVICE_RESPONSE_HEADER));
  if (ReceiveMessageRspHeader->Status != 0) {
    DEBUG ((DEBUG_ERROR, "%a: Failed with SendMessageRsp status: %x\n", __FUNCTION__, ReceiveMessageRspHeader->Status));
    Status = EFI_ABORTED;
    goto QuitVTPMContextRead;
  }

  // Process the data received
  HeaderLen = sizeof (TDVMCALL_SERVICE_RESPONSE_HEADER) + sizeof (RECEIVE_MESSAGE_RESPONSE_HEADER);
  DataLen   = TdvmcallRspHeader->Length - HeaderLen;
  if (DataLen > *ResponseSize) {
    DEBUG ((DEBUG_ERROR, "%a: Failed with DataLen too small\n", __FUNCTION__));
    *ResponseSize = DataLen;
    Status        = EFI_BUFFER_TOO_SMALL;
    goto QuitVTPMContextRead;
  }

  *ResponseSize = DataLen;
  Ptr           = RspBuffer + HeaderLen;
  CopyMem (Response, Ptr, DataLen);

  Status = EFI_SUCCESS;

QuitVTPMContextRead:
  if (CmdBuffer) {
    FreeSharedBuffer (CmdBuffer, VTPM_DEFAULT_ALLOCATION_PAGE);
  }

  if (RspBuffer) {
    FreeSharedBuffer (RspBuffer, VTPM_DEFAULT_ALLOCATION_PAGE);
  }

  return Status;
}
