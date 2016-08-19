/***********************************************************************
**
**  REBOL [R3] Language Interpreter and Run-time Environment
**
**  Copyright 2012 REBOL Technologies
**  REBOL is a trademark of REBOL Technologies
**
**  Licensed under the Apache License, Version 2.0 (the "License");
**  you may not use this file except in compliance with the License.
**  You may obtain a copy of the License at
**
**  http://www.apache.org/licenses/LICENSE-2.0
**
**  Unless required by applicable law or agreed to in writing, software
**  distributed under the License is distributed on an "AS IS" BASIS,
**  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
**  See the License for the specific language governing permissions and
**  limitations under the License.
**
************************************************************************
**
**  Title: Simple readline() line input handler
**  Author: Carl Sassenrath
**  Purpose:
**      Processes special keys for input line editing and recall.
**      Avoides use of complex OS libraries and GNU readline().
**      but hardcodes some parts only for the common standard.
**
**  Usage: This file is meant to be used in more than just REBOL, so
**  it does not include the normal REBOL header files, but rather
**  defines its own types and constants.
**
************************************************************************
**
**  NOTE to PROGRAMMERS:
**
**    1. Keep code clear and simple.
**    2. Document unusual code, reasoning, or gotchas.
**    3. Use same style for code, vars, indent(4), comments, etc.
**    4. Keep in mind Linux, OS X, BSD, big/little endian CPUs.
**    5. Test everything, then test it again.
**
************************************************************************
**
**  Naive UTF-8 support and additional movement keys
**  by Esgorhannoth, 2016
**  
**  Date: 2016/08/19
**
**  Keys added / modified:
**  Ctrl-D - added UTF-8 support
**  Ctrl-F / Ctrl-B - added UTF-8 support
**  Ctrl-N / Ctrl-P - history down / up
**  Ctrl-K - kill to the end of line
**  Ctrl-U - kill full line
**  Ctrl-W - kill word backward
**  Ctrl-T - transpose characters
**  Alt-F / Alt-B - move forward / backward by word
**  Alt-D  - kill word forward
**
***********************************************************************/

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

// #define TEST_MODE  // teset as stand-alone program

#ifdef NO_TTY_ATTRIBUTES
#ifdef TO_WIN32
#include <io.h>
#endif
#else
#include <termios.h>
#endif

#define FALSE 0
#define TRUE (0==0)

enum {
	BEL =   7,
	BS  =   8,
  TAB =   9,
	LF  =  10,
	CR  =  13,
	ESC =  27,
	DEL = 127,
};


// eSko |16.0812.1337|
// Unicode consts
#define UTF8_CONT_START 0x80
#define UTF8_CONT_END   0xBF
#define UTF8_L2_FROM    0xC0 // Code point with length of 2 bytes (L2) from 1100 0000 (C0)
#define UTF8_L2_TO      0xDF // to 1101 1111 (DF)
#define UTF8_L3_FROM    0xE0 // from 1110 0000 (E0)
#define UTF8_L3_TO      0xEF // to 1110 1111 (EF)
#define UTF8_L4_FROM    0xF0 // from 1111 0000 (F0)
#define UTF8_L4_TO      0xF7 // to 1111 0111 (F7)
// But if we need them in 2's negative compliment:
/*
#define UTF8_CONT_START -128 // -128
#define UTF8_CONT_END    -65 // 1011 1111 -> 0100 0000 + 1 -> 0100 0001 -> -65
#define UTF8_L2_FROM     -64 //  0xC0 -> 1100 0000 -> 0011 1111++ -> 0100 0000 -> -64
#define UTF8_L2_TO       -33 //  0xDF -> 1101 1111 -> 0010 0000++ -> 0010 0001 -> -33
#define UTF8_L3_FROM     -32 //  0xE0 -> 1110 0000 -> 0001 1111++ -> 0010 0000 -> -32
#define UTF8_L3_TO       -17 //  0xEF -> 1110 1111 -> 0001 0000++ -> 0001 0001 -> -17
#define UTF8_L4_FROM     -16 //  0xF0 -> 1111 0000 -> 0000 1111++ -> 0001 0000 -> -16
#define UTF8_L4_TO        -9 //  0xF7 -> 1111 0111 -> 0000 1000++ -> 0000 1001 ->  -9
*/

// Configuration:
#define TERM_BUF_LEN 4096	// chars allowed per line
#define READ_BUF_LEN 64		// chars per read()
#define MAX_HISTORY  300	// number of lines stored

// Macros: (does not use reb-c.h)
#define MAKE_STR(l) (char*)malloc(l)
#define WRITE_CHAR(s)    write(1, s, 1)
#define WRITE_CHARS(s,l) write(1, s, l)
#define WRITE_STR(s)     write(1, s, strlen(s))

#define DBG_INT(t,n) //printf("\r\ndbg[%s]: %d\r\n", t, (n));
#define DBG_STR(t,s) //printf("\r\ndbg[%s]: %s\r\n", t, (s));

typedef struct term_data {
	char *buffer;
	char *residue;
	char *out;
	int pos;
	int end;
	int hist;
} STD_TERM;

// Globals:
static int  Term_Init = 0;			// Terminal init was successful
static char **Line_History;		// Prior input lines
static int Line_Count;			// Number of prior lines

#ifndef NO_TTY_ATTRIBUTES
static struct termios Term_Attrs;	// Initial settings, restored on exit
#endif


/***********************************************************************
**
*/	STD_TERM *Init_Terminal(void)
/*
**		Change the terminal modes to those required for proper
**		REBOL console handling. Return TRUE on success.
**
***********************************************************************/
{
	STD_TERM *term;
#ifndef NO_TTY_ATTRIBUTES
	struct termios attrs;

	if (Term_Init || tcgetattr(0, &Term_Attrs)) return FALSE;

	attrs = Term_Attrs;

	// Local modes:
	attrs.c_lflag &= ~(ECHO | ICANON); // raw input

	// Input modes:
	attrs.c_iflag &= ~(ICRNL | INLCR); // leave CR an LF as is

	// Output modes:
	attrs.c_oflag |= ONLCR; // On output, emit CRLF

	// Special modes:
	attrs.c_cc[VMIN] = 1;	// min num of bytes for READ to return
	attrs.c_cc[VTIME] = 0;	// how long to wait for input

	tcsetattr(0, TCSADRAIN, &attrs);
#endif

	// Setup variables:
	Line_History = (char**)malloc((MAX_HISTORY+2) * sizeof(char*));
	Line_History[0] = "";
	Line_Count = 1;

	term = malloc(sizeof(*term));
	memset(term, 0, sizeof(*term));
	term->buffer = MAKE_STR(TERM_BUF_LEN);
	term->buffer[0] = 0;
	term->residue = MAKE_STR(TERM_BUF_LEN);
	term->residue[0] = 0;

	Term_Init = TRUE;

	return term;
}


/***********************************************************************
**
*/	void Quit_Terminal(STD_TERM *term)
/*
**		Restore the terminal modes original entry settings,
**		in preparation for exit from program.
**
***********************************************************************/
{
	int n;

	if (Term_Init) {
#ifndef NO_TTY_ATTRIBUTES
		tcsetattr(0, TCSADRAIN, &Term_Attrs);
#endif
		free(term->residue);
		free(term->buffer);
		free(term);
		for (n = 1; n < Line_Count; n++) free(Line_History[n]);
		free(Line_History);
	}

	Term_Init = FALSE;
}


/***********************************************************************
**
*/	static void Write_Char(char c, int n)
/*
**		Write out repeated number of chars.
**		Unicode: not used
**
***********************************************************************/
{
	char buf[4];

	buf[0] = c;
	for (; n > 0; n--) WRITE_CHAR(buf);
}


/***********************************************************************
**
/	static int UTF8_Bytes_Num(unsigned char *cp)
*/	static int UTF8_Bytes_Num(char *cp)
/*
**		Return number of bytes for given UTF8 codepoint
**
***********************************************************************/
{
  unsigned char *uc = cp;
  //printf("uc in bytes = %d\n", *uc);
  //printf("l2 st = %d\n", UTF8_L2_FROM);
  //printf("l2 en = %d\n", UTF8_L2_TO);
  switch( *uc ) {
    case UTF8_L2_FROM ... UTF8_L2_TO:
      return 2;
      break;
    case UTF8_L3_FROM ... UTF8_L3_TO:
      return 3;
      break;
    case UTF8_L4_FROM ... UTF8_L4_TO:
      return 4;
      break;
    default:
      return 1;
  }
}


/***********************************************************************
**
*/	static int Is_UTF8_Lead(unsigned char *cp)
/*
**		Check if given char is UTF-8 leading byte
**
***********************************************************************/
{
  return UTF8_Bytes_Num(cp) > 1;
}


/***********************************************************************
**
*/	static int Is_UTF8_Cont(unsigned char *cp)
/*
**		Check if given char is UTF-8 continuation
**
***********************************************************************/
{
  return  ( *cp >= UTF8_CONT_START ) && ( *cp <= UTF8_CONT_END );
}


/***********************************************************************
**
*/	static int Is_Space(unsigned char *cp)
/*
**		Check if given char is Space (32, 0x20)
**
***********************************************************************/
{
  return  *cp == 32;
}


/***********************************************************************
**
*/	static int UTF8_Prev(STD_TERM *term)
/*
**		Returns position of previous codepoint, be it UTF8 leading byte or not
**
***********************************************************************/
{
  if( term->pos <= 0 ) return 0; // Start of line

  int pos = term->pos;
  char *cp;
  do {
    // TODO No error handling, we assume that it's valid UTF8 everywhere
    pos--;
    cp = term->buffer + pos;
  } while( Is_UTF8_Cont(cp) );
  if( pos < 0 ) pos = 0; // TODO Do we need it?
  return pos;
}


/***********************************************************************
**
*/	static int UTF8_Next(STD_TERM *term)
/*
**		Returns position of next codepoint from current pos,
**    be it UTF8 leading byte or not
**
***********************************************************************/
{
  if( term->pos >= term->end ) return term->end; // End of line

  int pos = term->pos;
  char *cp;
  do {
    // TODO No error handling, we assume that it's valid UTF8 everywhere
    pos++;
    cp = term->buffer + pos;
  } while( Is_UTF8_Cont(cp) );
  if( pos > term->end ) pos = term->end; // TODO Do we need it?
  return pos;
}


/***********************************************************************
**
*/	static int UTF8_Next_From(STD_TERM *term, int cpos)
/*
**		Returns position of next codepoint from given position,
**    be it UTF8 leading byte or not
**
***********************************************************************/
{
  if( cpos >= term->end ) return term->end; // End of line

  int pos = cpos;
  char *cp;
  do {
    // TODO No error handling, we assume that it's valid UTF8 everywhere
    pos++;
    cp = term->buffer + pos;
  } while( pos < term->end && Is_UTF8_Cont(cp) );
  //if( pos > term->end ) pos = term->end; // TODO Do we need it?
  return pos;
}

/***********************************************************************
**
*/	int UTF8_CPs_Around_Cursor(STD_TERM *term, int whereto)
/*
**		Returns number of code points left or right of term->pos
**    whereto >= 0 - right of cursor pos
**    whereto <  0 - left of cursor pos
**
***********************************************************************/
{
  int count = 0, pos;
  if( whereto >= 0 ) {
    // right
    pos = term->pos;
    char *cp = term->buffer + pos;
    while( pos < term->end ) {
      count++;
      pos =  UTF8_Next_From(term, pos);
    }
  }
  else {
    // left
    pos = 0;
    char *cp = term->buffer;
    while( pos < term->pos ) {
      count++;
      pos =  UTF8_Next_From(term, pos);
    }
  }
  return count;
}


/***********************************************************************
**
*/	int UTF8_Tail_Len(STD_TERM *term)
/*
**		Returns number of code points from term->pos to term->end
**
***********************************************************************/
{
  int len = 0;
  int i = term->pos;
  for(; i < term->end; i++ )
    if( ! Is_UTF8_Cont( term->buffer + i ) ) len++;
  return len;
}


/***********************************************************************
**
*/	static void Store_Line(STD_TERM *term)
/*
**		Makes a copy of the current buffer and store it in the
**		history list. Returns the copied string.
**
***********************************************************************/
{
	term->buffer[term->end] = 0;
	term->out = MAKE_STR(term->end + 1);
	strcpy(term->out, term->buffer);

	// If max history, drop older lines (but not [0] empty line):
	if (Line_Count >= MAX_HISTORY) {
		free(Line_History[1]);
		memmove(Line_History+1, Line_History+2, (MAX_HISTORY-2)*sizeof(char*));
		Line_Count = MAX_HISTORY-1;
	}

	Line_History[Line_Count++] = term->out;
}


/***********************************************************************
**
*/	static void Recall_Line(STD_TERM *term)
/*
**		Set the current buffer to the contents of the history
**		list at its current position. Clip at the ends.
**		Return the history line index number.
**		Unicode: ok
**
***********************************************************************/
{
	if (term->hist < 0) term->hist = 0;

	if (term->hist == 0)
		Write_Char(BEL, 1); // bell

	if (term->hist >= Line_Count) {
		// Special case: no "next" line:
		term->hist = Line_Count;
		term->buffer[0] = 0;
		term->pos = term->end = 0;
	}
	else {
		// Fetch prior line:
		strcpy(term->buffer, Line_History[term->hist]);
		term->pos = term->end = strlen(term->buffer);
	}
}


/***********************************************************************
**
*/	static void Clear_Line(STD_TERM *term)
/*
**		Clear all the chars from the current position to the end.
**		Reset cursor to current position.
**		Unicode: not used
**
***********************************************************************/
{
	Write_Char(' ', term->end - term->pos); // wipe prior line
	Write_Char(BS, term->end - term->pos); // return to position
}


/***********************************************************************
**
*/	static void Home_Line(STD_TERM *term)
/*
**		Reset cursor to home position.
**		Unicode: ok (was 'not used' but it WAS acutally used, coz term->pos
          is number of bytes, not codepoints)
**
***********************************************************************/
{
  int i, len = 0;
  char *cp;
  //if( term->pos == term->end ) term->pos--;
  /*
  for(; term->pos > 0; term->pos--) {
    cp = term->buffer + term->pos;
    if( ! Is_UTF8_Cont(cp) ) len++;
  }
  */
  for( i=0 ; i < term->pos ; i++ ) {
    cp = term->buffer + i;
    if( ! Is_UTF8_Cont(cp) ) len++;
  }
  term->pos = 0;
	Write_Char(BS, len);
}


/***********************************************************************
**
*/	static void End_Line(STD_TERM *term)
/*
**		Move cursor to end position.
**		Unicode: not used
**
***********************************************************************/
{
	int len = term->end - term->pos;

	if (len > 0) {
		WRITE_CHARS(term->buffer+term->pos, len);
		term->pos = term->end;
	}
}


/***********************************************************************
**
*/	static void Show_Line(STD_TERM *term, int blanks)
/*
**		Refresh a line from the current position to the end.
**		Extra blanks can be specified to erase chars off end.
**		If blanks is negative, stay at end of line.
**		Reset the cursor back to current position.
**		Unicode: ok
**
***********************************************************************/
{
	int len;

	//printf("\r\nsho pos: %d end: %d ==", term->pos, term->end);

	// Clip bounds:
	if (term->pos < 0) term->pos = 0;
	else if (term->pos > term->end) term->pos = term->end;

  // TODO Temporary move left if current char is UTF8 continuation
  //int old_pos = term->pos;
  //while( Is_UTF8_Cont( term->buffer + term->pos ) ) term->pos--;


	if (blanks >= 0) {
		len = term->end - term->pos;
		//len = UTF8_Tail_Len(term);
    //printf("%d\n", len);
		WRITE_CHARS(term->buffer+term->pos, len);
	}
	else {
		WRITE_CHARS(term->buffer, term->end);
		blanks = -blanks;
		len = 0;
	}

	Write_Char(' ', blanks);
	// Write_Char(BS, blanks + len); // return to position or end
	Write_Char(BS, blanks + UTF8_Tail_Len(term)); // return to position or end

  // TODO restore
  //term->pos = old_pos;
}


/***********************************************************************
**
/	//static int Is_Valid_UTF8(unsigned char *cp)
*/	static int Is_Valid_UTF8(char *cp)
/*
**		Check if cp is followed by valid continuations
**		Unicode: ok
**
***********************************************************************/
{
  unsigned char *uc = cp;
  int n_cont = UTF8_Bytes_Num(cp);
  for(; n_cont > 1; n_cont-- )
    if( ! Is_UTF8_Cont(++uc) ) return FALSE;  // wasted 1.5 days coz of uc++ here
  return TRUE;
}


/***********************************************************************
**
*/	static char *Insert_Char(STD_TERM *term, char *cp)
/*
**		Insert a char at the current position. Adjust end position.
**		Redisplay the line.
**		Unicode: ok
**
***********************************************************************/
{
  int len = 1;
	//printf("\r\nins pos: %d end: %d ==", term->pos, term->end);
	if (term->end < TERM_BUF_LEN-1) { // avoid buffer overrun

    // UTF8
    if( *cp < 0 ) {
      if( Is_Valid_UTF8(cp) ) {
        len = UTF8_Bytes_Num(cp);
      }
    }

		if (term->pos < term->end) { // open space for it:
			memmove(term->buffer + term->pos + len, term->buffer + term->pos, 1 + term->end - term->pos);
		}

    // UTF8
    int i, pos = term->pos;
    if( *cp < 0 ) { // if UTF8
      for( i=0; i < len; i++ ) {
        term->buffer[pos++] = *cp++;
        WRITE_CHAR( term->buffer + pos - 1 );
      }
      term->pos += len;
      term->end += len;
    }
    else { // no UTF8 - works
      WRITE_CHAR(cp);
      term->buffer[term->pos] = *cp;
      term->pos++;
      term->end++;
      cp++;
    }
		Show_Line(term, 0);
	}

	return cp;
}


/***********************************************************************
**
*/	static void Delete_Char(STD_TERM *term, int back)
/*
**		Delete a char at the current position. Adjust end position.
**		Redisplay the line. Blank out extra char at end.
**		Unicode: ok
**
***********************************************************************/
{
	int len;

	if ( (term->pos == term->end) && back == 0) return; //Ctrl-D at EOL
	if ( (term->pos == 0) && back == 1) return; //BS and line start
   
  if( back ) term->pos = UTF8_Prev(term);

  char *cp = term->buffer + term->pos;
  int by = UTF8_Bytes_Num(cp);

	len = term->end - term->pos;

	if (term->pos >= 0 && len > 0) {
    // TODO watch out for `len` below
		memmove(term->buffer + term->pos, term->buffer + term->pos + by, len);
		if (back) Write_Char(BS, 1);
		term->end -= by;
		Show_Line(term, by);
	}
	else term->pos = 0;

  // orig
  /*
	if (back) term->pos--;

	len = 1 + term->end - term->pos; // "1 +" for final \0, I guess?

	if (term->pos >= 0 && len > 0) {
		memmove(term->buffer + term->pos, term->buffer + term->pos + 1, len);
		if (back) Write_Char(BS, 1);
		term->end--;
		Show_Line(term, 1);
	}
	else term->pos = 0;
  */
}


/***********************************************************************
**
*/	static void Kill_Line_End(STD_TERM *term)
/*
**		Deletes characters from pos included to term->end
**		Unicode: ok
**
***********************************************************************/
{
  int len = term->end - term->pos;
  term->buffer[term->pos] = '\0';
  term->end = term->pos;
  Show_Line(term, len);
}


/***********************************************************************
**
*/	static void Kill_Line(STD_TERM *term)
/*
**		Deletes characters from pos included to term->end
**		Unicode: ok
**
***********************************************************************/
{
  int n_bs = UTF8_CPs_Around_Cursor(term, -1); // left of cursor
  if( n_bs > 0 ) Write_Char(BS, n_bs);

  term->pos = 0;
  int len = UTF8_Tail_Len(term);

  Kill_Line_End(term);
  Show_Line(term, len);
}


/***********************************************************************
**
*/	static void Kill_Word(STD_TERM *term, int whereto)
/*
**		Delete word left or right of cursor pos.
**    whereto >= 0 - delete word right of cursor pos
**    whereto <  0 - delete word left of cursor pos
**		Unicode: ok
**
***********************************************************************/
{
  int met_non_space = FALSE;
  int tail_len = 0, n_cp = 0, pos = term->pos;
  char *cp;
  // count and set up values
  if( whereto >= 0 ) {
    // right
    // get pos of space after end of word or EOL
    while( pos < term->end ) {
      cp = term->buffer + pos;
      // if it's space and we started on space, that's it, leave
      if( Is_Space(cp) && met_non_space ) break;
      if( ! Is_Space(cp) ) met_non_space = TRUE;
      if( ! Is_UTF8_Cont(cp) ) n_cp++;
      pos++;
    }
    // byte length of tail after deletion
    tail_len = term->end - pos;
    // trim term->end
    term->end = term->end - (pos - term->pos);
  }
  else {
    // left
    // get pos of start of previous word or SOL
    while( pos > 0 ) {
      pos--;
      cp = term->buffer + pos;
      if( ! Is_UTF8_Cont(cp) ) n_cp++;
      if( ! Is_Space(cp) ) met_non_space = TRUE;
      
      // if it's space and we started on space, that's it, leave
      if( Is_Space(cp) && met_non_space ) {
        // compensate for space we break on
        pos++;
        n_cp--;
        break;
      }
    }
    if( n_cp > 0 )
      Write_Char(BS, n_cp); // if we kill left of cursor, move cursor

    // swap term->pos and pos for memmove to work as if
    // it's kill_word(right)
    // pos must always be greater then term->pos
    int tmp = pos;
    pos = term->pos;
    term->pos = tmp;
    
    // byte length of tail after deletion
    tail_len = term->end - pos;
    // trim term->end
    term->end = term->end - (pos - term->pos);
  }

  if( pos != term->pos ) { // if we actually killed some word
    memmove(
        term->buffer + term->pos,
        term->buffer + pos,
        tail_len+1); // +1 for \0

    Show_Line(term, n_cp);
  }
}


/***********************************************************************
**
*/	static void Move_Cursor(STD_TERM *term, int count)
/*
**		Move cursor right or left by one char.
**		Unicode: ok
**
***********************************************************************/
{
  int len;
	if (count < 0) {
		if (term->pos > 0) {
      term->pos--;
			Write_Char(BS, 1);
      while ( ( term-> pos > 0 ) &&
              ( Is_UTF8_Cont( term->buffer + term->pos ) )) {
        term->pos--;
      }
		}
	}
	else {
		if (term->pos < term->end) {
			WRITE_CHAR(term->buffer + term->pos);
			term->pos++;
      while( (term-> pos < term->end) &&
             ( Is_UTF8_Cont( term->buffer + term->pos ) )) {
        WRITE_CHAR(term->buffer + term->pos);
        term->pos++;
      }
		}
	}
}


/***********************************************************************
**
*/	static void Next_Word(STD_TERM *term, int whereto)
/*
**		Moves cursor to start of next word left/right of cursor
**    whereto >= 0 - moves cursor to the next word to the right
**    whereto <  0 - moves cursor to the next word to the left
**		Unicode: ok
**
***********************************************************************/
{
  int pos = term->pos, n_cp = 0;
  char *cp;
  if( whereto >= 0 ) {
    // right
    int met_space = FALSE;
    while( pos < term->end ) {
      cp = term->buffer + pos;
      if( ! Is_UTF8_Cont(cp) ) n_cp++;
      if( Is_Space(cp) ) met_space = TRUE;

      if( ! Is_Space(cp) && met_space ) break;
      pos++;
    }
    int len = pos - term->pos;
    WRITE_CHARS(term->buffer + term->pos, len);
    term->pos = pos;
  }
  else {
    // left
    int met_non_space = FALSE;
    while( pos > 0 ) {
      pos--;
      cp = term->buffer + pos;
      if( ! Is_UTF8_Cont(cp) ) n_cp++;
      if( ! Is_Space(cp) ) met_non_space = TRUE;

      if( Is_Space(cp) && met_non_space ) {
        // compensate for space we break on
        pos++;
        n_cp--;
        break;
      }
    } // while
    term->pos = pos;
    Write_Char(BS, n_cp);
  }
}


/***********************************************************************
**
*/	static History_Next(STD_TERM *term, int which)
/*
**		Moves up and down buffer history.
**    which >= 0 - move forward in time (recall newer line)
**    which <  0 - move backward in time (recall older line)
**		Unicode: ok
**
***********************************************************************/
{
  int len;
  if( which < 0 )
    term->hist--;
  else
    term->hist++;
  len = term->end;
  Home_Line(term);
  Recall_Line(term);
  if (len <= term->end) len = 0;
  else len = term->end - len;
  Show_Line(term, len-1); // len < 0 (stay at end)
}


/***********************************************************************
**
*/	static void Transpose_Chars(STD_TERM *term)
/*
**		Changes places of characters before and after cursor pos.
**		Unicode: ok
**
***********************************************************************/
{
  if( term->end <= 1 ) return;
  if( term->end > 1 && term->end <= 4 ) {
    // len is 1 to 4 bytes
    // It still may be just one UTF8 character
    int count = 0, i;
    char *cp;
    for( i = 0; i < term->end; i++ ) {
      cp = term->buffer + i;
      if( ! Is_UTF8_Cont(cp) ) count++;
    }
    if( count <= 1 ) return; // It's just one character
  }
  if( term->pos == term->end ) { // we're at EOL
    term->pos = UTF8_Prev(term);
    Write_Char(BS, 1);
  }
  if( term->pos == 0 ) { // we're at SOL
    int old_pos = term->pos;
    term->pos = UTF8_Next(term);
    WRITE_CHARS(term->buffer + term->pos, term->pos - old_pos);
  }

  // Let's do it...
  int pos1, pos2, len1, len2;
  char *buf = term->buffer;
  char ch1[4], ch2[4]; // temporary storage, no more then 4 bytes

  pos1 = UTF8_Prev(term);          // prev codepoint
  pos2 = term->pos;                // current codepoint
  len1 = pos2 - pos1;              // term->pos - pos1
  len2 = UTF8_Next(term) - pos2;   // next CP - current (term->pos) 
  // save original two chars
  strncpy(ch1, buf+pos1, len1);
  strncpy(ch2, buf+pos2, len2);
  // swap their places
  strncpy(buf+pos1, ch2, len2);
  strncpy(buf+pos1+len2, ch1, len1);
  // Move cursor one char left for refreshing
  Write_Char(BS, 1);
  // Refresh displaying of these two chars
  WRITE_CHARS(buf + pos1, len1+len2);
  // After copying term->pos may be illegal, i.e. inside UTF8 char
  term->pos = pos2+len2; // move it to pos after the second char
}


/***********************************************************************
**
*/	static char *Process_Key(STD_TERM *term, char *cp)
/*
**		Process the next key. If it's an edit key, perform the
**		necessary editing action. Return position of next char.
**		Unicode: ok
**
***********************************************************************/
{
	int len;

	if (*cp == 0) return cp;


	if (*cp == ESC) {
		// Escape sequence:
		cp++;

    // if Esc-UTF8 - ignore
    if( Is_UTF8_Lead(cp) ) {
      cp += UTF8_Bytes_Num(cp);
      return cp;
    }

    if( *cp >= 'a' && *cp <= 'z' ) {
      // Alt/Meta/Esc-a..z
      switch( *cp ) {
        case 'b':
          Next_Word(term, -1);
          break;
        case 'd':
          Kill_Word(term, 1);
          break;
        case 'f':
          Next_Word(term, 1);
          break;
      }
    }

		if (*cp == '[' || *cp == 'O') {

			// Special key:
			switch (*++cp) {

			// Arrow keys:
			case 'A':	// up arrow
        History_Next(term, -1);
				break;
			case 'B':	// down arrow
        History_Next(term, 1);
				break;

			case 'D':	// left arrow
				Move_Cursor(term, -1);
				break;
			case 'C':	// right arrow
				Move_Cursor(term, 1);
				break;

			// Other special keys:
			case '1':	// home
				Home_Line(term);
				cp++; // remove ~
				break;
			case '4':	// end
				End_Line(term);
				cp++; // remove ~
				break;
			case '3':	// delete
				Delete_Char(term, FALSE);
				cp++; // remove ~
				break;

			case 'H':	// home
				Home_Line(term);
				break;
			case 'F':	// end
				End_Line(term);
				break;

			default:
				//WRITE_STR("[ESC]");
				cp--;
			}
		}
		else {
			switch (*++cp) {
			case 'H':	// home
				Home_Line(term);
				break;
			case 'F':	// end
				End_Line(term);
				break;
			default:
				// Q: what other keys do we want to support ?!
				//WRITE_STR("[ESC]");
				cp--;
			}
		}
	}
	else {
		// ASCII char:
		switch (*cp) {

		case  BS:	// backspace
		case DEL:	// delete
			Delete_Char(term, TRUE);
			break;
    case TAB:
      break;

		case CR:	// CR
			if (cp[1] == LF) cp++; // eat
		case LF:	// LF
			WRITE_STR("\r\n");
			Store_Line(term);
			break;

		case 1:	// CTRL-A
			Home_Line(term);
			break;
		case 2:	// CTRL-B
			Move_Cursor(term, -1);
			break;
		case 4:	// CTRL-D
			Delete_Char(term, FALSE);
			break;
		case 5:	// CTRL-E
			End_Line(term);
			break;
		case 6:	// CTRL-F
			Move_Cursor(term, 1);
			break;
    case 11: // CTRL-K Delete to end of line
      Kill_Line_End(term);
      break;
    case 12: // CTRL-L
      break;
    case 14:	// Ctrl-N
      History_Next(term, 1);
      break;
    case 16:	// Ctrl-P
      History_Next(term, -1);
      break;
    case 20:  // Ctrl-T
      Transpose_Chars(term);
      break;
    case 21:  // Ctrl-U
      Kill_Line(term);
      break;
    case 23:  // Ctrl-W
      Kill_Word(term, -1);
      break;

    // Ignore cases
    case 15:
    case 17 ... 19:
    case 22:
    case 24 ... 26:
    case 28 ... 31:
      break;

		default:
			cp = Insert_Char(term, cp);
			cp--; // coz cp is now ++cp and with return we ++cp again
		}
	}

	return ++cp;
}


/***********************************************************************
**
*/	static int Read_Bytes(STD_TERM *term, char *buf, int len)
/*
**		Read the next "chunk" of data into the terminal buffer.
**
***********************************************************************/
{
	int end;

	// If we have leftovers:
	if (term->residue[0]) {
		end = strlen(term->residue);
		if (end < len) len = end;
		strncpy(buf, term->residue, len); // terminated below
		memmove(term->residue, term->residue+len, end-len); // remove
		term->residue[end-len] = 0;
	}
	else {
		// Read next few bytes. We don't know how many may be waiting.
		// We assume that escape-sequences are always complete in buf.
		// (No partial escapes.) If this is not true, then we will need
		// to add an additional "collection" loop here.
		if ((len = read(0, buf, len)) < 0) {
			WRITE_STR("\r\nI/O terminated\r\n");
			Quit_Terminal(term); // something went wrong
			exit(100);
		}
	}

	buf[len] = 0;
	buf[len+1] = 0;

	DBG_INT("read len", len);

	return len;
}


/***********************************************************************
**
*/	int Read_Line(STD_TERM *term, char *result, int limit)
/*
**		Read a line (as a sequence of bytes) from the terminal.
**		Handles line editing and line history recall.
**		Returns number of bytes in line.
**
***********************************************************************/
{
	char buf[READ_BUF_LEN];
	char *cp;
	int len;		// length of IO read

	term->pos = term->end = 0;
	term->hist = Line_Count;
	term->out = 0;
	term->buffer[0] = 0;

	do {
		Read_Bytes(term, buf, READ_BUF_LEN-2);
		for (cp = buf; *cp;) {
			cp = Process_Key(term, cp);
		}
	} while (!term->out);

	// Not at end of input? Save any unprocessed chars:
	if (*cp) {
		if (strlen(term->residue) + strlen(cp) < TERM_BUF_LEN-1) // avoid overrun
			strcat(term->residue, cp);
	}

	// Fill the output buffer:
	len = strlen(term->out);
	if (len >= limit-1) len = limit-2;
	strncpy(result, term->out, limit);
	result[len++] = LF;
	result[len] = 0;

	return len;
}

#ifdef TEST_MODE
test(STD_TERM *term, char *cp) {
	term->hist = Line_Count;
	term->pos = term->end = 0;
	term->out = 0;
	term->buffer[0] = 0;
	while (*cp) cp = Process_Key(term, cp);
}

main() {
	int i;
	char buf[1024];
	STD_TERM *term;

	term = Init_Terminal();

	Write_Char('-', 50);
	WRITE_STR("\r\n");

#ifdef WIN32
	test(term, "text\010\010st\n"); //bs bs
	test(term, "test\001xxxx\n"); // home
	test(term, "test\001\005xxxx\n"); // home
	test(term, "\033[A\n"); // up arrow
#endif

	do {
		WRITE_STR("Жожоба >> ");
		i = Read_Line(term, buf, 1000);
		printf("len: %d %s\r\n", i, term->out);
    if( strcmp(term->out, "quit") == 0 ) break;
	} while (i > 0);

	Quit_Terminal(term);
  exit(0);
}
#endif
