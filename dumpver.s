; quickie tool to dump the skunkboard rev and serial to the console
; intended for use from JCP
; mac -fb dumpver.s
; aln -w -e -v -rd -a 5000 x 4000 -o dumpver.cof dumpver.o

;; Like the ROM Dump, this code is really, really dumb, expecting 
;; to run from the skunkboard startup. It won't even init hardware
;; or set up the OPL, since that's all technically done.
;; All it's going to do is use the utilities to read the version
;; info vectors, format them, and print them to console.

	.include "jaguar.inc"
	
start:
	; prepare the library
	jsr skunkRESET
	
	; start here - console must be open (jcp's job)
	move.l	$800800,d1		; version 00.ma.mi.re
	move.l	$800808,d2		; serial
	
	; now format the string
	move.l	#buf,a0			; address of string
	add.l	#13,a0			; address of first digit
	
	move.l	d1,d0
	lsr.l	#8,d0			; major
	lsr.l	#8,d0			; major
	and.l	#$ff,d0
	jsr		writehex

	addq	#1,a0
	move.l	d1,d0
	lsr.l	#8,d0			; minor
	and.l	#$ff,d0
	jsr		writehex
	
	addq	#1,a0
	move.l	d1,d0
	and.l	#$ff,d0			; rev
	jsr		writehex
	
	add.l	#9,a0			; address for serial
	move.l	d2,d0
	jsr		writeser

	; now output the string
	move.l	#buf,a0
	jsr skunkCONSOLEWRITE
	
	; make sure the close is not interpreted before the write
	jsr	skunkNOP

	; now close the console, which will exit JCP and make us reset
	jsr skunkCONSOLECLOSE

	; wait to be reset
	move.w #$12dd,BG

forevr:
	nop
	jmp forevr
	
	; write a 2 character hex value from d0 to a0
writehex:
	move.l	d0,d3
	lsr.l	#4,d3
	and.l	#$F,d3
	cmp.l	#$10,d3
	blt		.lt
	addq	#7,d3
.lt:
	add.l	#48,d3
	move.b	d3,(a0)+

	move.l	d0,d3
	and.l	#$F,d3
	cmp.l	#$10,d3
	blt		.lt2
	addq	#7,d3
.lt2:
	add.l	#48,d3
	move.b	d3,(a0)+
	rts

	; write a 4-digit hex value from d0 to a0
writeser:
	and.l	#$ffff,d0	; mask for range
	move.l	d0,d4
	lsr.l	#8,d0
	jsr		writehex
	move.l	d4,d0
	jsr		writehex
	rts

	.long	
buf:
	.dc.b	'Boot version xx.xx.xx, Serial xxxx',13,0

	.long
	.include "skunk.s"

	.end
