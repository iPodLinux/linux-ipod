/*
 * Initially derived from Hyperstone's E1 processor initialization
 * and modified accordingly.
 *
 * Copyright (C) 2002-2003 GDT, George Thanos <george.thanos@gdt.gr>
 * 			        Yannis Mitsos <yannis.mitsos@gdt.gr>
 * Copyright (C) March 2002 Hyperstone AG
 */

.macro  _STH
        ori   SR, 0x20
.endm

.global	__rambase
.global	__ramstart
.global	__ramend
	.data
	.balign	4
__rambase:
		  .long	 0
__ramstart:
		  .long  0
__ramend:
		  .long  0
	.balign	16 

.global __init_reg_stack

	.global	Priority	; Task-Priority
	.global	_Stack1Size	; Size of hardware stack
	.global	_Stack2Size	; Size of aggregate stack
	.global	_Stack1Base	; Base of hardware stack
	.global	_Stack2Base	; Base of aggregate stack
	.global	_Mem0HeapBase	; Base of Heap in Mem0
	.global	_Mem1HeapBase	; Base of Heap in Mem1

//	.global	_main
	.global	Main

	.text
BasePtrs:
	.weak	G6Base,G7Base,G8Base,G9Base,G10Base,G11Base,G12Base,G13Base
	.long	G6Base,G7Base,G8Base,G9Base,G10Base,G11Base,G12Base,G13Base
BasePtrsEnd:

;============================================================================;
; Startup-Code                                                               ;
;============================================================================;
	.text
Main:
//StartUp:
_main:
_start:
	frame	L8, L0
	mov L1, SR
	movi	L2, __bss_start	; clear the .bss segment
0:	cmpi	L2, __bss_end
	bhe	0f
	dbr	0b
	stw.p	L2, 0
0:	sum     L2, PC, BasePtrs-$	; Load BasePtrs G6-G13

	ldd.p   L2, G6			; Zero G6-G13
	ldd.p   L2, G8
	ldw.p   L2, G11
	ldd.p   L2, G12

_linux_kernel_startup_initialization:
	# load _rambase, _ramstart, _ramend
	movi	L1, __rambase
	movi	L2, __sdata
	stw.n	L1, L2, 4
	movi	L2, __end
	stw.n	L1, L2, 4
	movi	L2, ___ramend
	stw.n	L1, L2, 4
	# load _current_task
	movi	L1, __current_task
	movi	L2, _init_task_union
	stw.r	L1, L2


# Aggregate Stack for Init Task
	movi    G9, _init_task_union
	sum	G9, G9, 0x2000
	mov 	G3, G9
	mov     G4, G9
	sum	G5, G9, -0x2000		; 2 * PAGE_SIZE

# Register Stack for Init Task
	movi	G11, __Init_Register_Stack_
	_STH
	movi	SP, __Init_Register_Stack_        # SP
	movi    L2, __Init_Register_Stack_
	addi    L2, 0x2000
	_STH
	mov	UB, L2				  # UB
	mov	L1, SR
	andni	L1, 0xffe00000 # Clear FP, FL
	ori	L1, 0x00400000 # Set FL=2
	movi	L2, __Init_Register_Stack_        # SP
	andni	L2, 0xfffffe03 # Get FP (bits 2..8) from SP
	shli	L2, 23         # Shift left by 23 to match SRs FP
	or	L1, L2         # Get the new FP to saved SR
	movi	L0, __init_reg_stack
	ori     L0, 0x1
	ret	PC, L0

__init_reg_stack:
	call	L2, 0, _start_kernel

	.section _GOT_
	.long   _start+4		; OnCreate
	.long   _start+8		; OnError
	.long   BasePtrs	; G6
	.long   BasePtrs+4	; G7
	.long   BasePtrs+8	; G8
	
	.END
