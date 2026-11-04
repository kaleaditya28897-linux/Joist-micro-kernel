/* A Bison parser, made by GNU Bison 3.8.2.  */

/* Bison interface for Yacc-like parsers in C

   Copyright (C) 1984, 1989-1990, 2000-2015, 2018-2021 Free Software Foundation,
   Inc.

   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <https://www.gnu.org/licenses/>.  */

/* As a special exception, you may create a larger work that contains
   part or all of the Bison parser skeleton and distribute that work
   under terms of your choice, so long as that work isn't itself a
   parser generator using the skeleton or a modified version thereof
   as a parser skeleton.  Alternatively, if you modify or redistribute
   the parser skeleton itself, you may (at your option) remove this
   special exception, which will cause the skeleton and the resulting
   Bison output files to be licensed under the GNU General Public
   License without this special exception.

   This special exception was added by the Free Software Foundation in
   version 2.2 of Bison.  */

/* DO NOT RELY ON FEATURES THAT ARE NOT DOCUMENTED in the manual,
   especially those whose name start with YY_ or yy_.  They are
   private implementation details that can be changed or removed.  */

#ifndef YY_YY_PARSER_H_INCLUDED
# define YY_YY_PARSER_H_INCLUDED
/* Debug traces.  */
#ifndef YYDEBUG
# define YYDEBUG 0
#endif
#if YYDEBUG
extern int yydebug;
#endif

/* Token kinds.  */
#ifndef YYTOKENTYPE
# define YYTOKENTYPE
  enum yytokentype
  {
    YYEMPTY = -2,
    YYEOF = 0,                     /* "end of file"  */
    YYerror = 256,                 /* error  */
    YYUNDEF = 257,                 /* "invalid token"  */
    sySkip = 258,                  /* sySkip  */
    syRoutine = 259,               /* syRoutine  */
    sySimpleRoutine = 260,         /* sySimpleRoutine  */
    sySubsystem = 261,             /* sySubsystem  */
    syKernelUser = 262,            /* syKernelUser  */
    syKernelServer = 263,          /* syKernelServer  */
    syMsgOption = 264,             /* syMsgOption  */
    syMsgSeqno = 265,              /* syMsgSeqno  */
    syWaitTime = 266,              /* syWaitTime  */
    syNoWaitTime = 267,            /* syNoWaitTime  */
    syServerPrefix = 268,          /* syServerPrefix  */
    syUserPrefix = 269,            /* syUserPrefix  */
    syServerDemux = 270,           /* syServerDemux  */
    syRCSId = 271,                 /* syRCSId  */
    syImport = 272,                /* syImport  */
    syUImport = 273,               /* syUImport  */
    sySImport = 274,               /* sySImport  */
    syIn = 275,                    /* syIn  */
    syOut = 276,                   /* syOut  */
    syInOut = 277,                 /* syInOut  */
    syRequestPort = 278,           /* syRequestPort  */
    syReplyPort = 279,             /* syReplyPort  */
    sySReplyPort = 280,            /* sySReplyPort  */
    syUReplyPort = 281,            /* syUReplyPort  */
    syType = 282,                  /* syType  */
    syArray = 283,                 /* syArray  */
    syStruct = 284,                /* syStruct  */
    syOf = 285,                    /* syOf  */
    syInTran = 286,                /* syInTran  */
    syOutTran = 287,               /* syOutTran  */
    syDestructor = 288,            /* syDestructor  */
    syCType = 289,                 /* syCType  */
    syCUserType = 290,             /* syCUserType  */
    syCServerType = 291,           /* syCServerType  */
    syCString = 292,               /* syCString  */
    syColon = 293,                 /* syColon  */
    sySemi = 294,                  /* sySemi  */
    syComma = 295,                 /* syComma  */
    syPlus = 296,                  /* syPlus  */
    syMinus = 297,                 /* syMinus  */
    syStar = 298,                  /* syStar  */
    syDiv = 299,                   /* syDiv  */
    syLParen = 300,                /* syLParen  */
    syRParen = 301,                /* syRParen  */
    syEqual = 302,                 /* syEqual  */
    syCaret = 303,                 /* syCaret  */
    syTilde = 304,                 /* syTilde  */
    syLAngle = 305,                /* syLAngle  */
    syRAngle = 306,                /* syRAngle  */
    syLBrack = 307,                /* syLBrack  */
    syRBrack = 308,                /* syRBrack  */
    syLCBrack = 309,               /* syLCBrack  */
    syRCBrack = 310,               /* syRCBrack  */
    syBar = 311,                   /* syBar  */
    syError = 312,                 /* syError  */
    syNumber = 313,                /* syNumber  */
    sySymbolicType = 314,          /* sySymbolicType  */
    syIdentifier = 315,            /* syIdentifier  */
    syString = 316,                /* syString  */
    syQString = 317,               /* syQString  */
    syFileName = 318,              /* syFileName  */
    syIPCFlag = 319,               /* syIPCFlag  */
    syInTranPayload = 320          /* syInTranPayload  */
  };
  typedef enum yytokentype yytoken_kind_t;
#endif
/* Token kinds.  */
#define YYEMPTY -2
#define YYEOF 0
#define YYerror 256
#define YYUNDEF 257
#define sySkip 258
#define syRoutine 259
#define sySimpleRoutine 260
#define sySubsystem 261
#define syKernelUser 262
#define syKernelServer 263
#define syMsgOption 264
#define syMsgSeqno 265
#define syWaitTime 266
#define syNoWaitTime 267
#define syServerPrefix 268
#define syUserPrefix 269
#define syServerDemux 270
#define syRCSId 271
#define syImport 272
#define syUImport 273
#define sySImport 274
#define syIn 275
#define syOut 276
#define syInOut 277
#define syRequestPort 278
#define syReplyPort 279
#define sySReplyPort 280
#define syUReplyPort 281
#define syType 282
#define syArray 283
#define syStruct 284
#define syOf 285
#define syInTran 286
#define syOutTran 287
#define syDestructor 288
#define syCType 289
#define syCUserType 290
#define syCServerType 291
#define syCString 292
#define syColon 293
#define sySemi 294
#define syComma 295
#define syPlus 296
#define syMinus 297
#define syStar 298
#define syDiv 299
#define syLParen 300
#define syRParen 301
#define syEqual 302
#define syCaret 303
#define syTilde 304
#define syLAngle 305
#define syRAngle 306
#define syLBrack 307
#define syRBrack 308
#define syLCBrack 309
#define syRCBrack 310
#define syBar 311
#define syError 312
#define syNumber 313
#define sySymbolicType 314
#define syIdentifier 315
#define syString 316
#define syQString 317
#define syFileName 318
#define syIPCFlag 319
#define syInTranPayload 320

/* Value type.  */
#if ! defined YYSTYPE && ! defined YYSTYPE_IS_DECLARED
union YYSTYPE
{
#line 141 "parser.y"

    u_int number;
    identifier_t identifier;
    const_string_t string;
    statement_kind_t statement_kind;
    ipc_type_t *type;
    struct
    {
	u_int innumber;		/* msgt_name value, when sending */
	const_string_t instr;
	u_int outnumber;	/* msgt_name value, when receiving */
	const_string_t outstr;
	u_int size;		/* 0 means there is no default size */
    } symtype;
    /* Holds information about a structure while parsing. */
    struct
    {
        /* The required alignment (in bytes) so far. */
        u_int type_alignment_in_bytes;
        /* The size of the struct in bytes so far. */
        u_int size_in_bytes;
    } structured_type;
    routine_t *routine;
    arg_kind_t direction;
    argument_t *argument;
    ipc_flags_t flag;

#line 225 "parser.h"

};
typedef union YYSTYPE YYSTYPE;
# define YYSTYPE_IS_TRIVIAL 1
# define YYSTYPE_IS_DECLARED 1
#endif


extern YYSTYPE yylval;


int yyparse (void);


#endif /* !YY_YY_PARSER_H_INCLUDED  */
