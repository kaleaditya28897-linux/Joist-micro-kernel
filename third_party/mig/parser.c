/* A Bison parser, made by GNU Bison 3.8.2.  */

/* Bison implementation for Yacc-like parsers in C

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

/* C LALR(1) parser skeleton written by Richard Stallman, by
   simplifying the original so-called "semantic" parser.  */

/* DO NOT RELY ON FEATURES THAT ARE NOT DOCUMENTED in the manual,
   especially those whose name start with YY_ or yy_.  They are
   private implementation details that can be changed or removed.  */

/* All symbols defined below should begin with yy or YY, to avoid
   infringing on user name space.  This should be done even for local
   variables, as they might otherwise be expanded by user macros.
   There are some unavoidable exceptions within include files to
   define necessary library symbols; they are noted "INFRINGES ON
   USER NAME SPACE" below.  */

/* Identify Bison output, and Bison version.  */
#define YYBISON 30802

/* Bison version string.  */
#define YYBISON_VERSION "3.8.2"

/* Skeleton name.  */
#define YYSKELETON_NAME "yacc.c"

/* Pure parsers.  */
#define YYPURE 0

/* Push parsers.  */
#define YYPUSH 0

/* Pull parsers.  */
#define YYPULL 1




/* First part of user prologue.  */
#line 117 "parser.y"


#include <stdio.h>

#include "cpu.h"
#include "error.h"
#include "lexxer.h"
#include "global.h"
#include "mig_string.h"
#include "type.h"
#include "routine.h"
#include "statement.h"
#include "utils.h"

static const char *import_name(statement_kind_t sk);

void
yyerror(const char *s)
{
    error("%s", s);
}

#line 94 "parser.c"

# ifndef YY_CAST
#  ifdef __cplusplus
#   define YY_CAST(Type, Val) static_cast<Type> (Val)
#   define YY_REINTERPRET_CAST(Type, Val) reinterpret_cast<Type> (Val)
#  else
#   define YY_CAST(Type, Val) ((Type) (Val))
#   define YY_REINTERPRET_CAST(Type, Val) ((Type) (Val))
#  endif
# endif
# ifndef YY_NULLPTR
#  if defined __cplusplus
#   if 201103L <= __cplusplus
#    define YY_NULLPTR nullptr
#   else
#    define YY_NULLPTR 0
#   endif
#  else
#   define YY_NULLPTR ((void*)0)
#  endif
# endif

/* Use api.header.include to #include this header
   instead of duplicating it here.  */
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

#line 305 "parser.c"

};
typedef union YYSTYPE YYSTYPE;
# define YYSTYPE_IS_TRIVIAL 1
# define YYSTYPE_IS_DECLARED 1
#endif


extern YYSTYPE yylval;


int yyparse (void);


#endif /* !YY_YY_PARSER_H_INCLUDED  */
/* Symbol kind.  */
enum yysymbol_kind_t
{
  YYSYMBOL_YYEMPTY = -2,
  YYSYMBOL_YYEOF = 0,                      /* "end of file"  */
  YYSYMBOL_YYerror = 1,                    /* error  */
  YYSYMBOL_YYUNDEF = 2,                    /* "invalid token"  */
  YYSYMBOL_sySkip = 3,                     /* sySkip  */
  YYSYMBOL_syRoutine = 4,                  /* syRoutine  */
  YYSYMBOL_sySimpleRoutine = 5,            /* sySimpleRoutine  */
  YYSYMBOL_sySubsystem = 6,                /* sySubsystem  */
  YYSYMBOL_syKernelUser = 7,               /* syKernelUser  */
  YYSYMBOL_syKernelServer = 8,             /* syKernelServer  */
  YYSYMBOL_syMsgOption = 9,                /* syMsgOption  */
  YYSYMBOL_syMsgSeqno = 10,                /* syMsgSeqno  */
  YYSYMBOL_syWaitTime = 11,                /* syWaitTime  */
  YYSYMBOL_syNoWaitTime = 12,              /* syNoWaitTime  */
  YYSYMBOL_syServerPrefix = 13,            /* syServerPrefix  */
  YYSYMBOL_syUserPrefix = 14,              /* syUserPrefix  */
  YYSYMBOL_syServerDemux = 15,             /* syServerDemux  */
  YYSYMBOL_syRCSId = 16,                   /* syRCSId  */
  YYSYMBOL_syImport = 17,                  /* syImport  */
  YYSYMBOL_syUImport = 18,                 /* syUImport  */
  YYSYMBOL_sySImport = 19,                 /* sySImport  */
  YYSYMBOL_syIn = 20,                      /* syIn  */
  YYSYMBOL_syOut = 21,                     /* syOut  */
  YYSYMBOL_syInOut = 22,                   /* syInOut  */
  YYSYMBOL_syRequestPort = 23,             /* syRequestPort  */
  YYSYMBOL_syReplyPort = 24,               /* syReplyPort  */
  YYSYMBOL_sySReplyPort = 25,              /* sySReplyPort  */
  YYSYMBOL_syUReplyPort = 26,              /* syUReplyPort  */
  YYSYMBOL_syType = 27,                    /* syType  */
  YYSYMBOL_syArray = 28,                   /* syArray  */
  YYSYMBOL_syStruct = 29,                  /* syStruct  */
  YYSYMBOL_syOf = 30,                      /* syOf  */
  YYSYMBOL_syInTran = 31,                  /* syInTran  */
  YYSYMBOL_syOutTran = 32,                 /* syOutTran  */
  YYSYMBOL_syDestructor = 33,              /* syDestructor  */
  YYSYMBOL_syCType = 34,                   /* syCType  */
  YYSYMBOL_syCUserType = 35,               /* syCUserType  */
  YYSYMBOL_syCServerType = 36,             /* syCServerType  */
  YYSYMBOL_syCString = 37,                 /* syCString  */
  YYSYMBOL_syColon = 38,                   /* syColon  */
  YYSYMBOL_sySemi = 39,                    /* sySemi  */
  YYSYMBOL_syComma = 40,                   /* syComma  */
  YYSYMBOL_syPlus = 41,                    /* syPlus  */
  YYSYMBOL_syMinus = 42,                   /* syMinus  */
  YYSYMBOL_syStar = 43,                    /* syStar  */
  YYSYMBOL_syDiv = 44,                     /* syDiv  */
  YYSYMBOL_syLParen = 45,                  /* syLParen  */
  YYSYMBOL_syRParen = 46,                  /* syRParen  */
  YYSYMBOL_syEqual = 47,                   /* syEqual  */
  YYSYMBOL_syCaret = 48,                   /* syCaret  */
  YYSYMBOL_syTilde = 49,                   /* syTilde  */
  YYSYMBOL_syLAngle = 50,                  /* syLAngle  */
  YYSYMBOL_syRAngle = 51,                  /* syRAngle  */
  YYSYMBOL_syLBrack = 52,                  /* syLBrack  */
  YYSYMBOL_syRBrack = 53,                  /* syRBrack  */
  YYSYMBOL_syLCBrack = 54,                 /* syLCBrack  */
  YYSYMBOL_syRCBrack = 55,                 /* syRCBrack  */
  YYSYMBOL_syBar = 56,                     /* syBar  */
  YYSYMBOL_syError = 57,                   /* syError  */
  YYSYMBOL_syNumber = 58,                  /* syNumber  */
  YYSYMBOL_sySymbolicType = 59,            /* sySymbolicType  */
  YYSYMBOL_syIdentifier = 60,              /* syIdentifier  */
  YYSYMBOL_syString = 61,                  /* syString  */
  YYSYMBOL_syQString = 62,                 /* syQString  */
  YYSYMBOL_syFileName = 63,                /* syFileName  */
  YYSYMBOL_syIPCFlag = 64,                 /* syIPCFlag  */
  YYSYMBOL_syInTranPayload = 65,           /* syInTranPayload  */
  YYSYMBOL_YYACCEPT = 66,                  /* $accept  */
  YYSYMBOL_Statements = 67,                /* Statements  */
  YYSYMBOL_Statement = 68,                 /* Statement  */
  YYSYMBOL_Subsystem = 69,                 /* Subsystem  */
  YYSYMBOL_SubsystemStart = 70,            /* SubsystemStart  */
  YYSYMBOL_SubsystemMods = 71,             /* SubsystemMods  */
  YYSYMBOL_SubsystemMod = 72,              /* SubsystemMod  */
  YYSYMBOL_SubsystemName = 73,             /* SubsystemName  */
  YYSYMBOL_SubsystemBase = 74,             /* SubsystemBase  */
  YYSYMBOL_MsgOption = 75,                 /* MsgOption  */
  YYSYMBOL_WaitTime = 76,                  /* WaitTime  */
  YYSYMBOL_ServerPrefix = 77,              /* ServerPrefix  */
  YYSYMBOL_UserPrefix = 78,                /* UserPrefix  */
  YYSYMBOL_ServerDemux = 79,               /* ServerDemux  */
  YYSYMBOL_Import = 80,                    /* Import  */
  YYSYMBOL_ImportIndicant = 81,            /* ImportIndicant  */
  YYSYMBOL_RCSDecl = 82,                   /* RCSDecl  */
  YYSYMBOL_TypeDecl = 83,                  /* TypeDecl  */
  YYSYMBOL_NamedTypeSpec = 84,             /* NamedTypeSpec  */
  YYSYMBOL_TransTypeSpec = 85,             /* TransTypeSpec  */
  YYSYMBOL_TypeSpec = 86,                  /* TypeSpec  */
  YYSYMBOL_StructList = 87,                /* StructList  */
  YYSYMBOL_BasicTypeSpec = 88,             /* BasicTypeSpec  */
  YYSYMBOL_IPCFlags = 89,                  /* IPCFlags  */
  YYSYMBOL_PrimIPCType = 90,               /* PrimIPCType  */
  YYSYMBOL_IPCType = 91,                   /* IPCType  */
  YYSYMBOL_PrevTypeSpec = 92,              /* PrevTypeSpec  */
  YYSYMBOL_VarArrayHead = 93,              /* VarArrayHead  */
  YYSYMBOL_ArrayHead = 94,                 /* ArrayHead  */
  YYSYMBOL_StructHead = 95,                /* StructHead  */
  YYSYMBOL_CStringSpec = 96,               /* CStringSpec  */
  YYSYMBOL_IntExp = 97,                    /* IntExp  */
  YYSYMBOL_RoutineDecl = 98,               /* RoutineDecl  */
  YYSYMBOL_Routine = 99,                   /* Routine  */
  YYSYMBOL_SimpleRoutine = 100,            /* SimpleRoutine  */
  YYSYMBOL_Arguments = 101,                /* Arguments  */
  YYSYMBOL_ArgumentList = 102,             /* ArgumentList  */
  YYSYMBOL_Argument = 103,                 /* Argument  */
  YYSYMBOL_Direction = 104,                /* Direction  */
  YYSYMBOL_ArgumentType = 105,             /* ArgumentType  */
  YYSYMBOL_LookString = 106,               /* LookString  */
  YYSYMBOL_LookFileName = 107,             /* LookFileName  */
  YYSYMBOL_LookQString = 108               /* LookQString  */
};
typedef enum yysymbol_kind_t yysymbol_kind_t;




#ifdef short
# undef short
#endif

/* On compilers that do not define __PTRDIFF_MAX__ etc., make sure
   <limits.h> and (if available) <stdint.h> are included
   so that the code can choose integer types of a good width.  */

#ifndef __PTRDIFF_MAX__
# include <limits.h> /* INFRINGES ON USER NAME SPACE */
# if defined __STDC_VERSION__ && 199901 <= __STDC_VERSION__
#  include <stdint.h> /* INFRINGES ON USER NAME SPACE */
#  define YY_STDINT_H
# endif
#endif

/* Narrow types that promote to a signed type and that can represent a
   signed or unsigned integer of at least N bits.  In tables they can
   save space and decrease cache pressure.  Promoting to a signed type
   helps avoid bugs in integer arithmetic.  */

#ifdef __INT_LEAST8_MAX__
typedef __INT_LEAST8_TYPE__ yytype_int8;
#elif defined YY_STDINT_H
typedef int_least8_t yytype_int8;
#else
typedef signed char yytype_int8;
#endif

#ifdef __INT_LEAST16_MAX__
typedef __INT_LEAST16_TYPE__ yytype_int16;
#elif defined YY_STDINT_H
typedef int_least16_t yytype_int16;
#else
typedef short yytype_int16;
#endif

/* Work around bug in HP-UX 11.23, which defines these macros
   incorrectly for preprocessor constants.  This workaround can likely
   be removed in 2023, as HPE has promised support for HP-UX 11.23
   (aka HP-UX 11i v2) only through the end of 2022; see Table 2 of
   <https://h20195.www2.hpe.com/V2/getpdf.aspx/4AA4-7673ENW.pdf>.  */
#ifdef __hpux
# undef UINT_LEAST8_MAX
# undef UINT_LEAST16_MAX
# define UINT_LEAST8_MAX 255
# define UINT_LEAST16_MAX 65535
#endif

#if defined __UINT_LEAST8_MAX__ && __UINT_LEAST8_MAX__ <= __INT_MAX__
typedef __UINT_LEAST8_TYPE__ yytype_uint8;
#elif (!defined __UINT_LEAST8_MAX__ && defined YY_STDINT_H \
       && UINT_LEAST8_MAX <= INT_MAX)
typedef uint_least8_t yytype_uint8;
#elif !defined __UINT_LEAST8_MAX__ && UCHAR_MAX <= INT_MAX
typedef unsigned char yytype_uint8;
#else
typedef short yytype_uint8;
#endif

#if defined __UINT_LEAST16_MAX__ && __UINT_LEAST16_MAX__ <= __INT_MAX__
typedef __UINT_LEAST16_TYPE__ yytype_uint16;
#elif (!defined __UINT_LEAST16_MAX__ && defined YY_STDINT_H \
       && UINT_LEAST16_MAX <= INT_MAX)
typedef uint_least16_t yytype_uint16;
#elif !defined __UINT_LEAST16_MAX__ && USHRT_MAX <= INT_MAX
typedef unsigned short yytype_uint16;
#else
typedef int yytype_uint16;
#endif

#ifndef YYPTRDIFF_T
# if defined __PTRDIFF_TYPE__ && defined __PTRDIFF_MAX__
#  define YYPTRDIFF_T __PTRDIFF_TYPE__
#  define YYPTRDIFF_MAXIMUM __PTRDIFF_MAX__
# elif defined PTRDIFF_MAX
#  ifndef ptrdiff_t
#   include <stddef.h> /* INFRINGES ON USER NAME SPACE */
#  endif
#  define YYPTRDIFF_T ptrdiff_t
#  define YYPTRDIFF_MAXIMUM PTRDIFF_MAX
# else
#  define YYPTRDIFF_T long
#  define YYPTRDIFF_MAXIMUM LONG_MAX
# endif
#endif

#ifndef YYSIZE_T
# ifdef __SIZE_TYPE__
#  define YYSIZE_T __SIZE_TYPE__
# elif defined size_t
#  define YYSIZE_T size_t
# elif defined __STDC_VERSION__ && 199901 <= __STDC_VERSION__
#  include <stddef.h> /* INFRINGES ON USER NAME SPACE */
#  define YYSIZE_T size_t
# else
#  define YYSIZE_T unsigned
# endif
#endif

#define YYSIZE_MAXIMUM                                  \
  YY_CAST (YYPTRDIFF_T,                                 \
           (YYPTRDIFF_MAXIMUM < YY_CAST (YYSIZE_T, -1)  \
            ? YYPTRDIFF_MAXIMUM                         \
            : YY_CAST (YYSIZE_T, -1)))

#define YYSIZEOF(X) YY_CAST (YYPTRDIFF_T, sizeof (X))


/* Stored state numbers (used for stacks). */
typedef yytype_uint8 yy_state_t;

/* State numbers in computations.  */
typedef int yy_state_fast_t;

#ifndef YY_
# if defined YYENABLE_NLS && YYENABLE_NLS
#  if ENABLE_NLS
#   include <libintl.h> /* INFRINGES ON USER NAME SPACE */
#   define YY_(Msgid) dgettext ("bison-runtime", Msgid)
#  endif
# endif
# ifndef YY_
#  define YY_(Msgid) Msgid
# endif
#endif


#ifndef YY_ATTRIBUTE_PURE
# if defined __GNUC__ && 2 < __GNUC__ + (96 <= __GNUC_MINOR__)
#  define YY_ATTRIBUTE_PURE __attribute__ ((__pure__))
# else
#  define YY_ATTRIBUTE_PURE
# endif
#endif

#ifndef YY_ATTRIBUTE_UNUSED
# if defined __GNUC__ && 2 < __GNUC__ + (7 <= __GNUC_MINOR__)
#  define YY_ATTRIBUTE_UNUSED __attribute__ ((__unused__))
# else
#  define YY_ATTRIBUTE_UNUSED
# endif
#endif

/* Suppress unused-variable warnings by "using" E.  */
#if ! defined lint || defined __GNUC__
# define YY_USE(E) ((void) (E))
#else
# define YY_USE(E) /* empty */
#endif

/* Suppress an incorrect diagnostic about yylval being uninitialized.  */
#if defined __GNUC__ && ! defined __ICC && 406 <= __GNUC__ * 100 + __GNUC_MINOR__
# if __GNUC__ * 100 + __GNUC_MINOR__ < 407
#  define YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN                           \
    _Pragma ("GCC diagnostic push")                                     \
    _Pragma ("GCC diagnostic ignored \"-Wuninitialized\"")
# else
#  define YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN                           \
    _Pragma ("GCC diagnostic push")                                     \
    _Pragma ("GCC diagnostic ignored \"-Wuninitialized\"")              \
    _Pragma ("GCC diagnostic ignored \"-Wmaybe-uninitialized\"")
# endif
# define YY_IGNORE_MAYBE_UNINITIALIZED_END      \
    _Pragma ("GCC diagnostic pop")
#else
# define YY_INITIAL_VALUE(Value) Value
#endif
#ifndef YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN
# define YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN
# define YY_IGNORE_MAYBE_UNINITIALIZED_END
#endif
#ifndef YY_INITIAL_VALUE
# define YY_INITIAL_VALUE(Value) /* Nothing. */
#endif

#if defined __cplusplus && defined __GNUC__ && ! defined __ICC && 6 <= __GNUC__
# define YY_IGNORE_USELESS_CAST_BEGIN                          \
    _Pragma ("GCC diagnostic push")                            \
    _Pragma ("GCC diagnostic ignored \"-Wuseless-cast\"")
# define YY_IGNORE_USELESS_CAST_END            \
    _Pragma ("GCC diagnostic pop")
#endif
#ifndef YY_IGNORE_USELESS_CAST_BEGIN
# define YY_IGNORE_USELESS_CAST_BEGIN
# define YY_IGNORE_USELESS_CAST_END
#endif


#define YY_ASSERT(E) ((void) (0 && (E)))

#if !defined yyoverflow

/* The parser invokes alloca or malloc; define the necessary symbols.  */

# ifdef YYSTACK_USE_ALLOCA
#  if YYSTACK_USE_ALLOCA
#   ifdef __GNUC__
#    define YYSTACK_ALLOC __builtin_alloca
#   elif defined __BUILTIN_VA_ARG_INCR
#    include <alloca.h> /* INFRINGES ON USER NAME SPACE */
#   elif defined _AIX
#    define YYSTACK_ALLOC __alloca
#   elif defined _MSC_VER
#    include <malloc.h> /* INFRINGES ON USER NAME SPACE */
#    define alloca _alloca
#   else
#    define YYSTACK_ALLOC alloca
#    if ! defined _ALLOCA_H && ! defined EXIT_SUCCESS
#     include <stdlib.h> /* INFRINGES ON USER NAME SPACE */
      /* Use EXIT_SUCCESS as a witness for stdlib.h.  */
#     ifndef EXIT_SUCCESS
#      define EXIT_SUCCESS 0
#     endif
#    endif
#   endif
#  endif
# endif

# ifdef YYSTACK_ALLOC
   /* Pacify GCC's 'empty if-body' warning.  */
#  define YYSTACK_FREE(Ptr) do { /* empty */; } while (0)
#  ifndef YYSTACK_ALLOC_MAXIMUM
    /* The OS might guarantee only one guard page at the bottom of the stack,
       and a page size can be as small as 4096 bytes.  So we cannot safely
       invoke alloca (N) if N exceeds 4096.  Use a slightly smaller number
       to allow for a few compiler-allocated temporary stack slots.  */
#   define YYSTACK_ALLOC_MAXIMUM 4032 /* reasonable circa 2006 */
#  endif
# else
#  define YYSTACK_ALLOC YYMALLOC
#  define YYSTACK_FREE YYFREE
#  ifndef YYSTACK_ALLOC_MAXIMUM
#   define YYSTACK_ALLOC_MAXIMUM YYSIZE_MAXIMUM
#  endif
#  if (defined __cplusplus && ! defined EXIT_SUCCESS \
       && ! ((defined YYMALLOC || defined malloc) \
             && (defined YYFREE || defined free)))
#   include <stdlib.h> /* INFRINGES ON USER NAME SPACE */
#   ifndef EXIT_SUCCESS
#    define EXIT_SUCCESS 0
#   endif
#  endif
#  ifndef YYMALLOC
#   define YYMALLOC malloc
#   if ! defined malloc && ! defined EXIT_SUCCESS
void *malloc (YYSIZE_T); /* INFRINGES ON USER NAME SPACE */
#   endif
#  endif
#  ifndef YYFREE
#   define YYFREE free
#   if ! defined free && ! defined EXIT_SUCCESS
void free (void *); /* INFRINGES ON USER NAME SPACE */
#   endif
#  endif
# endif
#endif /* !defined yyoverflow */

#if (! defined yyoverflow \
     && (! defined __cplusplus \
         || (defined YYSTYPE_IS_TRIVIAL && YYSTYPE_IS_TRIVIAL)))

/* A type that is properly aligned for any stack member.  */
union yyalloc
{
  yy_state_t yyss_alloc;
  YYSTYPE yyvs_alloc;
};

/* The size of the maximum gap between one aligned stack and the next.  */
# define YYSTACK_GAP_MAXIMUM (YYSIZEOF (union yyalloc) - 1)

/* The size of an array large to enough to hold all stacks, each with
   N elements.  */
# define YYSTACK_BYTES(N) \
     ((N) * (YYSIZEOF (yy_state_t) + YYSIZEOF (YYSTYPE)) \
      + YYSTACK_GAP_MAXIMUM)

# define YYCOPY_NEEDED 1

/* Relocate STACK from its old location to the new one.  The
   local variables YYSIZE and YYSTACKSIZE give the old and new number of
   elements in the stack, and YYPTR gives the new location of the
   stack.  Advance YYPTR to a properly aligned location for the next
   stack.  */
# define YYSTACK_RELOCATE(Stack_alloc, Stack)                           \
    do                                                                  \
      {                                                                 \
        YYPTRDIFF_T yynewbytes;                                         \
        YYCOPY (&yyptr->Stack_alloc, Stack, yysize);                    \
        Stack = &yyptr->Stack_alloc;                                    \
        yynewbytes = yystacksize * YYSIZEOF (*Stack) + YYSTACK_GAP_MAXIMUM; \
        yyptr += yynewbytes / YYSIZEOF (*yyptr);                        \
      }                                                                 \
    while (0)

#endif

#if defined YYCOPY_NEEDED && YYCOPY_NEEDED
/* Copy COUNT objects from SRC to DST.  The source and destination do
   not overlap.  */
# ifndef YYCOPY
#  if defined __GNUC__ && 1 < __GNUC__
#   define YYCOPY(Dst, Src, Count) \
      __builtin_memcpy (Dst, Src, YY_CAST (YYSIZE_T, (Count)) * sizeof (*(Src)))
#  else
#   define YYCOPY(Dst, Src, Count)              \
      do                                        \
        {                                       \
          YYPTRDIFF_T yyi;                      \
          for (yyi = 0; yyi < (Count); yyi++)   \
            (Dst)[yyi] = (Src)[yyi];            \
        }                                       \
      while (0)
#  endif
# endif
#endif /* !YYCOPY_NEEDED */

/* YYFINAL -- State number of the termination state.  */
#define YYFINAL  2
/* YYLAST -- Last index in YYTABLE.  */
#define YYLAST   218

/* YYNTOKENS -- Number of terminals.  */
#define YYNTOKENS  66
/* YYNNTS -- Number of nonterminals.  */
#define YYNNTS  43
/* YYNRULES -- Number of rules.  */
#define YYNRULES  104
/* YYNSTATES -- Number of states.  */
#define YYNSTATES  209

/* YYMAXUTOK -- Last valid token kind.  */
#define YYMAXUTOK   320


/* YYTRANSLATE(TOKEN-NUM) -- Symbol number corresponding to TOKEN-NUM
   as returned by yylex, with out-of-bounds checking.  */
#define YYTRANSLATE(YYX)                                \
  (0 <= (YYX) && (YYX) <= YYMAXUTOK                     \
   ? YY_CAST (yysymbol_kind_t, yytranslate[YYX])        \
   : YYSYMBOL_YYUNDEF)

/* YYTRANSLATE[TOKEN-NUM] -- Symbol number corresponding to TOKEN-NUM
   as returned by yylex.  */
static const yytype_int8 yytranslate[] =
{
       0,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     1,     2,     3,     4,
       5,     6,     7,     8,     9,    10,    11,    12,    13,    14,
      15,    16,    17,    18,    19,    20,    21,    22,    23,    24,
      25,    26,    27,    28,    29,    30,    31,    32,    33,    34,
      35,    36,    37,    38,    39,    40,    41,    42,    43,    44,
      45,    46,    47,    48,    49,    50,    51,    52,    53,    54,
      55,    56,    57,    58,    59,    60,    61,    62,    63,    64,
      65
};

#if YYDEBUG
/* YYRLINE[YYN] -- Source line where rule number YYN was defined.  */
static const yytype_int16 yyrline[] =
{
       0,   171,   171,   172,   175,   176,   177,   178,   179,   180,
     181,   182,   192,   194,   196,   197,   198,   199,   203,   217,
     229,   230,   233,   239,   247,   250,   253,   270,   276,   284,
     292,   300,   308,   319,   320,   321,   324,   334,   344,   348,
     350,   370,   385,   405,   420,   434,   443,   455,   457,   459,
     461,   463,   465,   467,   469,   473,   486,   503,   509,   519,
     520,   527,   536,   542,   546,   548,   572,   576,   578,   580,
     585,   589,   593,   595,   600,   602,   604,   606,   608,   610,
     615,   616,   619,   623,   627,   629,   634,   636,   643,   653,
     654,   655,   656,   657,   658,   659,   660,   661,   662,   663,
     666,   672,   677,   681,   685
};
#endif

/** Accessing symbol of state STATE.  */
#define YY_ACCESSING_SYMBOL(State) YY_CAST (yysymbol_kind_t, yystos[State])

#if YYDEBUG || 0
/* The user-facing name of the symbol whose (internal) number is
   YYSYMBOL.  No bounds checking.  */
static const char *yysymbol_name (yysymbol_kind_t yysymbol) YY_ATTRIBUTE_UNUSED;

/* YYTNAME[SYMBOL-NUM] -- String name of the symbol SYMBOL-NUM.
   First, the terminals, then, starting at YYNTOKENS, nonterminals.  */
static const char *const yytname[] =
{
  "\"end of file\"", "error", "\"invalid token\"", "sySkip", "syRoutine",
  "sySimpleRoutine", "sySubsystem", "syKernelUser", "syKernelServer",
  "syMsgOption", "syMsgSeqno", "syWaitTime", "syNoWaitTime",
  "syServerPrefix", "syUserPrefix", "syServerDemux", "syRCSId", "syImport",
  "syUImport", "sySImport", "syIn", "syOut", "syInOut", "syRequestPort",
  "syReplyPort", "sySReplyPort", "syUReplyPort", "syType", "syArray",
  "syStruct", "syOf", "syInTran", "syOutTran", "syDestructor", "syCType",
  "syCUserType", "syCServerType", "syCString", "syColon", "sySemi",
  "syComma", "syPlus", "syMinus", "syStar", "syDiv", "syLParen",
  "syRParen", "syEqual", "syCaret", "syTilde", "syLAngle", "syRAngle",
  "syLBrack", "syRBrack", "syLCBrack", "syRCBrack", "syBar", "syError",
  "syNumber", "sySymbolicType", "syIdentifier", "syString", "syQString",
  "syFileName", "syIPCFlag", "syInTranPayload", "$accept", "Statements",
  "Statement", "Subsystem", "SubsystemStart", "SubsystemMods",
  "SubsystemMod", "SubsystemName", "SubsystemBase", "MsgOption",
  "WaitTime", "ServerPrefix", "UserPrefix", "ServerDemux", "Import",
  "ImportIndicant", "RCSDecl", "TypeDecl", "NamedTypeSpec",
  "TransTypeSpec", "TypeSpec", "StructList", "BasicTypeSpec", "IPCFlags",
  "PrimIPCType", "IPCType", "PrevTypeSpec", "VarArrayHead", "ArrayHead",
  "StructHead", "CStringSpec", "IntExp", "RoutineDecl", "Routine",
  "SimpleRoutine", "Arguments", "ArgumentList", "Argument", "Direction",
  "ArgumentType", "LookString", "LookFileName", "LookQString", YY_NULLPTR
};

static const char *
yysymbol_name (yysymbol_kind_t yysymbol)
{
  return yytname[yysymbol];
}
#endif

#define YYPACT_NINF (-112)

#define yypact_value_is_default(Yyn) \
  ((Yyn) == YYPACT_NINF)

#define YYTABLE_NINF (-105)

#define yytable_value_is_error(Yyn) \
  0

/* YYPACT[STATE-NUM] -- Index in YYTABLE of the portion describing
   STATE-NUM.  */
static const yytype_int16 yypact[] =
{
    -112,    54,  -112,   -12,   -38,    42,    45,  -112,  -112,    56,
      80,    81,    82,  -112,  -112,     9,  -112,   100,   104,   105,
     106,   107,   108,   109,   110,   111,  -112,  -112,    90,   116,
     102,  -112,  -112,   112,   113,   113,  -112,  -112,  -112,   114,
    -112,  -112,    -4,  -112,  -112,  -112,  -112,  -112,  -112,  -112,
    -112,  -112,    91,    92,  -112,  -112,  -112,    93,    95,  -112,
      65,  -112,  -112,    55,  -112,  -112,  -112,  -112,    96,  -112,
    -112,  -112,  -112,  -112,  -112,  -112,  -112,  -112,  -112,  -112,
    -112,  -112,  -112,  -112,   117,   120,   115,   103,    10,   118,
     -41,    55,  -112,  -112,  -112,   -26,  -112,  -112,   121,  -112,
    -112,    55,    55,    55,  -112,  -112,  -112,  -112,    12,   122,
     -29,   -33,   119,   -32,   124,  -112,   127,   128,   129,   130,
     131,   133,   134,   -41,  -112,  -112,  -112,  -112,   123,   -23,
     -33,   132,  -112,     8,    53,   125,    57,   135,    66,   -33,
     126,   136,   137,   138,   139,   140,   141,  -112,   114,  -112,
    -112,   -33,   144,    36,  -112,   -33,   -33,   -33,   -33,   146,
     148,   142,  -112,   143,   -33,  -112,    87,   145,   147,   149,
    -112,  -112,  -112,   150,   151,    79,  -112,  -112,    94,    94,
    -112,  -112,  -112,  -112,  -112,   153,    83,    58,   157,   159,
     152,  -112,   154,   158,  -112,  -112,  -112,   155,   156,   160,
     161,  -112,   162,   163,  -112,   164,  -112,  -112,  -112
};

/* YYDEFACT[STATE-NUM] -- Default reduction number in state STATE-NUM.
   Performed when YYTABLE does not specify something else to do.  Zero
   means the default is an error.  */
static const yytype_int8 yydefact[] =
{
       2,     0,     1,     0,     0,     0,     0,    19,    28,     0,
       0,     0,     0,    16,     3,     0,    20,     0,     0,     0,
       0,     0,     0,     0,     0,     0,    80,    81,     0,     0,
       0,    17,    12,     0,     0,     0,    29,    30,    31,     0,
      37,     4,     0,     6,     5,     7,     8,     9,    14,    15,
      10,    11,     0,     0,    33,    34,    35,     0,     0,    13,
      89,    82,    83,     0,    22,    23,    24,    21,     0,    26,
      27,    32,    36,    98,    99,    97,    90,    91,    92,    93,
      94,    95,    96,    84,     0,    86,     0,     0,     0,     0,
       0,     0,    62,    63,    66,    38,    39,    47,    64,    57,
      48,     0,     0,     0,    54,    25,    18,    85,    89,     0,
       0,     0,     0,     0,     0,    51,     0,     0,     0,     0,
       0,     0,     0,     0,    49,    50,    52,    87,     0,     0,
       0,     0,    78,     0,     0,     0,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     0,     0,    65,   100,   101,
      59,     0,     0,     0,    67,     0,     0,     0,     0,     0,
       0,     0,    53,     0,     0,    72,    59,     0,     0,     0,
      44,    45,    46,     0,    88,     0,    68,    79,    74,    75,
      76,    77,    70,    71,    55,     0,     0,     0,     0,     0,
       0,    41,     0,     0,    56,    73,    58,     0,     0,     0,
      60,    69,     0,     0,    43,     0,    40,    42,    61
};

/* YYPGOTO[NTERM-NUM].  */
static const yytype_int16 yypgoto[] =
{
    -112,  -112,  -112,  -112,  -112,  -112,  -112,  -112,  -112,  -112,
    -112,  -112,  -112,  -112,  -112,  -112,  -112,  -112,    52,  -112,
     -60,  -112,  -112,    16,    61,    97,  -112,  -112,  -112,  -112,
    -112,  -111,  -112,  -112,  -112,   176,    85,  -112,  -112,  -112,
    -112,  -112,  -112
};

/* YYDEFGOTO[NTERM-NUM].  */
static const yytype_uint8 yydefgoto[] =
{
       0,     1,    14,    15,    16,    42,    67,    68,   106,    17,
      18,    19,    20,    21,    22,    57,    23,    24,    40,    95,
      96,   136,    97,   174,    98,    99,   100,   101,   102,   103,
     104,   133,    25,    26,    27,    61,    84,    85,    86,   150,
      28,    29,    30
};

/* YYTABLE[YYPACT[STATE-NUM]] -- What to do in state STATE-NUM.  If
   positive, shift that token.  If negative, reduce the rule whose
   number is the opposite.  If YYTABLE_NINF, syntax error.  */
static const yytype_int16 yytable[] =
{
     134,    32,   138,    64,    65,   116,   117,   118,   119,   120,
     121,   137,   130,   130,   129,   151,   130,    92,    93,   153,
      33,    73,    74,    75,   131,   132,   132,    31,   166,   132,
     152,   115,    76,    77,    78,    79,    80,    81,    82,   122,
     175,   124,   125,   126,   178,   179,   180,   181,    41,   155,
     156,   157,   158,   186,     2,     3,    66,     4,     5,     6,
       7,   159,   111,  -102,   112,  -102,     8,     9,    10,    11,
    -104,  -103,  -103,  -103,    73,    74,    75,   155,   156,   157,
     158,    12,   177,    87,    88,    76,    77,    78,    79,    80,
      81,    82,    89,    13,   155,   156,   157,   158,   192,    52,
      90,    53,    34,    91,   196,    35,   160,   155,   156,   157,
     158,    83,   162,    92,    93,    94,    36,   163,    58,   165,
     155,   156,   157,   158,   155,   156,   157,   158,   155,   156,
     157,   158,   193,    54,    55,    56,   195,   157,   158,    43,
      37,    38,    39,    44,    45,    46,    47,    48,    49,    50,
      51,    59,    69,    70,   105,   110,    71,    72,    60,   108,
     128,    63,   154,   107,   139,   140,   141,   142,   143,   144,
     113,   145,   146,   164,   176,   109,   182,   123,   183,   135,
     149,   184,   187,   148,   147,   161,   167,   114,   201,     0,
       0,   192,   194,   127,   190,     0,   168,   169,   170,   171,
     172,   173,   197,   185,   198,   188,   204,   189,   206,   207,
     191,    62,   199,   205,     0,   202,   203,   208,   200
};

static const yytype_int16 yycheck[] =
{
     111,    39,   113,     7,     8,    31,    32,    33,    34,    35,
      36,    43,    45,    45,    43,    38,    45,    58,    59,   130,
      58,     9,    10,    11,    53,    58,    58,    39,   139,    58,
      53,    91,    20,    21,    22,    23,    24,    25,    26,    65,
     151,   101,   102,   103,   155,   156,   157,   158,    39,    41,
      42,    43,    44,   164,     0,     1,    60,     3,     4,     5,
       6,    53,    52,     9,    54,    11,    12,    13,    14,    15,
      16,    17,    18,    19,     9,    10,    11,    41,    42,    43,
      44,    27,    46,    28,    29,    20,    21,    22,    23,    24,
      25,    26,    37,    39,    41,    42,    43,    44,    40,     9,
      45,    11,    60,    48,    46,    60,    53,    41,    42,    43,
      44,    46,    55,    58,    59,    60,    60,    60,    16,    53,
      41,    42,    43,    44,    41,    42,    43,    44,    41,    42,
      43,    44,    53,    17,    18,    19,    53,    43,    44,    39,
      60,    60,    60,    39,    39,    39,    39,    39,    39,    39,
      39,    39,    61,    61,    58,    52,    63,    62,    45,    39,
      38,    47,    30,    46,    40,    38,    38,    38,    38,    38,
      52,    38,    38,    38,    30,    60,    30,    56,    30,    60,
     128,    39,   166,    60,   123,    60,    60,    90,    30,    -1,
      -1,    40,    39,   108,    45,    -1,    60,    60,    60,    60,
      60,    60,    45,    60,    45,    60,    46,    60,    46,    46,
      60,    35,    60,    52,    -1,    60,    60,    53,    64
};

/* YYSTOS[STATE-NUM] -- The symbol kind of the accessing symbol of
   state STATE-NUM.  */
static const yytype_int8 yystos[] =
{
       0,    67,     0,     1,     3,     4,     5,     6,    12,    13,
      14,    15,    27,    39,    68,    69,    70,    75,    76,    77,
      78,    79,    80,    82,    83,    98,    99,   100,   106,   107,
     108,    39,    39,    58,    60,    60,    60,    60,    60,    60,
      84,    39,    71,    39,    39,    39,    39,    39,    39,    39,
      39,    39,     9,    11,    17,    18,    19,    81,    16,    39,
      45,   101,   101,    47,     7,     8,    60,    72,    73,    61,
      61,    63,    62,     9,    10,    11,    20,    21,    22,    23,
      24,    25,    26,    46,   102,   103,   104,    28,    29,    37,
      45,    48,    58,    59,    60,    85,    86,    88,    90,    91,
      92,    93,    94,    95,    96,    58,    74,    46,    39,    60,
      52,    52,    54,    52,    91,    86,    31,    32,    33,    34,
      35,    36,    65,    56,    86,    86,    86,   102,    38,    43,
      45,    53,    58,    97,    97,    60,    87,    43,    97,    40,
      38,    38,    38,    38,    38,    38,    38,    90,    60,    84,
     105,    38,    53,    97,    30,    41,    42,    43,    44,    53,
      53,    60,    55,    60,    38,    53,    97,    60,    60,    60,
      60,    60,    60,    60,    89,    97,    30,    46,    97,    97,
      97,    97,    30,    30,    39,    60,    97,    89,    60,    60,
      45,    60,    40,    53,    39,    53,    46,    45,    45,    60,
      64,    30,    60,    60,    46,    52,    46,    46,    53
};

/* YYR1[RULE-NUM] -- Symbol kind of the left-hand side of rule RULE-NUM.  */
static const yytype_int8 yyr1[] =
{
       0,    66,    67,    67,    68,    68,    68,    68,    68,    68,
      68,    68,    68,    68,    68,    68,    68,    68,    69,    70,
      71,    71,    72,    72,    73,    74,    75,    76,    76,    77,
      78,    79,    80,    81,    81,    81,    82,    83,    84,    85,
      85,    85,    85,    85,    85,    85,    85,    86,    86,    86,
      86,    86,    86,    86,    86,    87,    87,    88,    88,    89,
      89,    89,    90,    90,    91,    91,    92,    93,    93,    93,
      94,    95,    96,    96,    97,    97,    97,    97,    97,    97,
      98,    98,    99,   100,   101,   101,   102,   102,   103,   104,
     104,   104,   104,   104,   104,   104,   104,   104,   104,   104,
     105,   105,   106,   107,   108
};

/* YYR2[RULE-NUM] -- Number of symbols on the right-hand side of rule RULE-NUM.  */
static const yytype_int8 yyr2[] =
{
       0,     2,     0,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     3,     2,     2,     1,     2,     4,     1,
       0,     2,     1,     1,     1,     1,     3,     3,     1,     2,
       2,     2,     3,     1,     1,     1,     3,     2,     3,     1,
       8,     5,     8,     7,     4,     4,     4,     1,     1,     2,
       2,     2,     2,     4,     1,     3,     4,     1,     6,     0,
       3,     5,     1,     1,     1,     3,     1,     4,     5,     7,
       5,     5,     4,     6,     3,     3,     3,     3,     1,     3,
       1,     1,     3,     3,     2,     3,     1,     3,     5,     0,
       1,     1,     1,     1,     1,     1,     1,     1,     1,     1,
       1,     1,     0,     0,     0
};


enum { YYENOMEM = -2 };

#define yyerrok         (yyerrstatus = 0)
#define yyclearin       (yychar = YYEMPTY)

#define YYACCEPT        goto yyacceptlab
#define YYABORT         goto yyabortlab
#define YYERROR         goto yyerrorlab
#define YYNOMEM         goto yyexhaustedlab


#define YYRECOVERING()  (!!yyerrstatus)

#define YYBACKUP(Token, Value)                                    \
  do                                                              \
    if (yychar == YYEMPTY)                                        \
      {                                                           \
        yychar = (Token);                                         \
        yylval = (Value);                                         \
        YYPOPSTACK (yylen);                                       \
        yystate = *yyssp;                                         \
        goto yybackup;                                            \
      }                                                           \
    else                                                          \
      {                                                           \
        yyerror (YY_("syntax error: cannot back up")); \
        YYERROR;                                                  \
      }                                                           \
  while (0)

/* Backward compatibility with an undocumented macro.
   Use YYerror or YYUNDEF. */
#define YYERRCODE YYUNDEF


/* Enable debugging if requested.  */
#if YYDEBUG

# ifndef YYFPRINTF
#  include <stdio.h> /* INFRINGES ON USER NAME SPACE */
#  define YYFPRINTF fprintf
# endif

# define YYDPRINTF(Args)                        \
do {                                            \
  if (yydebug)                                  \
    YYFPRINTF Args;                             \
} while (0)




# define YY_SYMBOL_PRINT(Title, Kind, Value, Location)                    \
do {                                                                      \
  if (yydebug)                                                            \
    {                                                                     \
      YYFPRINTF (stderr, "%s ", Title);                                   \
      yy_symbol_print (stderr,                                            \
                  Kind, Value); \
      YYFPRINTF (stderr, "\n");                                           \
    }                                                                     \
} while (0)


/*-----------------------------------.
| Print this symbol's value on YYO.  |
`-----------------------------------*/

static void
yy_symbol_value_print (FILE *yyo,
                       yysymbol_kind_t yykind, YYSTYPE const * const yyvaluep)
{
  FILE *yyoutput = yyo;
  YY_USE (yyoutput);
  if (!yyvaluep)
    return;
  YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN
  YY_USE (yykind);
  YY_IGNORE_MAYBE_UNINITIALIZED_END
}


/*---------------------------.
| Print this symbol on YYO.  |
`---------------------------*/

static void
yy_symbol_print (FILE *yyo,
                 yysymbol_kind_t yykind, YYSTYPE const * const yyvaluep)
{
  YYFPRINTF (yyo, "%s %s (",
             yykind < YYNTOKENS ? "token" : "nterm", yysymbol_name (yykind));

  yy_symbol_value_print (yyo, yykind, yyvaluep);
  YYFPRINTF (yyo, ")");
}

/*------------------------------------------------------------------.
| yy_stack_print -- Print the state stack from its BOTTOM up to its |
| TOP (included).                                                   |
`------------------------------------------------------------------*/

static void
yy_stack_print (yy_state_t *yybottom, yy_state_t *yytop)
{
  YYFPRINTF (stderr, "Stack now");
  for (; yybottom <= yytop; yybottom++)
    {
      int yybot = *yybottom;
      YYFPRINTF (stderr, " %d", yybot);
    }
  YYFPRINTF (stderr, "\n");
}

# define YY_STACK_PRINT(Bottom, Top)                            \
do {                                                            \
  if (yydebug)                                                  \
    yy_stack_print ((Bottom), (Top));                           \
} while (0)


/*------------------------------------------------.
| Report that the YYRULE is going to be reduced.  |
`------------------------------------------------*/

static void
yy_reduce_print (yy_state_t *yyssp, YYSTYPE *yyvsp,
                 int yyrule)
{
  int yylno = yyrline[yyrule];
  int yynrhs = yyr2[yyrule];
  int yyi;
  YYFPRINTF (stderr, "Reducing stack by rule %d (line %d):\n",
             yyrule - 1, yylno);
  /* The symbols being reduced.  */
  for (yyi = 0; yyi < yynrhs; yyi++)
    {
      YYFPRINTF (stderr, "   $%d = ", yyi + 1);
      yy_symbol_print (stderr,
                       YY_ACCESSING_SYMBOL (+yyssp[yyi + 1 - yynrhs]),
                       &yyvsp[(yyi + 1) - (yynrhs)]);
      YYFPRINTF (stderr, "\n");
    }
}

# define YY_REDUCE_PRINT(Rule)          \
do {                                    \
  if (yydebug)                          \
    yy_reduce_print (yyssp, yyvsp, Rule); \
} while (0)

/* Nonzero means print parse trace.  It is left uninitialized so that
   multiple parsers can coexist.  */
int yydebug;
#else /* !YYDEBUG */
# define YYDPRINTF(Args) ((void) 0)
# define YY_SYMBOL_PRINT(Title, Kind, Value, Location)
# define YY_STACK_PRINT(Bottom, Top)
# define YY_REDUCE_PRINT(Rule)
#endif /* !YYDEBUG */


/* YYINITDEPTH -- initial size of the parser's stacks.  */
#ifndef YYINITDEPTH
# define YYINITDEPTH 200
#endif

/* YYMAXDEPTH -- maximum size the stacks can grow to (effective only
   if the built-in stack extension method is used).

   Do not make this value too large; the results are undefined if
   YYSTACK_ALLOC_MAXIMUM < YYSTACK_BYTES (YYMAXDEPTH)
   evaluated with infinite-precision integer arithmetic.  */

#ifndef YYMAXDEPTH
# define YYMAXDEPTH 10000
#endif






/*-----------------------------------------------.
| Release the memory associated to this symbol.  |
`-----------------------------------------------*/

static void
yydestruct (const char *yymsg,
            yysymbol_kind_t yykind, YYSTYPE *yyvaluep)
{
  YY_USE (yyvaluep);
  if (!yymsg)
    yymsg = "Deleting";
  YY_SYMBOL_PRINT (yymsg, yykind, yyvaluep, yylocationp);

  YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN
  YY_USE (yykind);
  YY_IGNORE_MAYBE_UNINITIALIZED_END
}


/* Lookahead token kind.  */
int yychar;

/* The semantic value of the lookahead symbol.  */
YYSTYPE yylval;
/* Number of syntax errors so far.  */
int yynerrs;




/*----------.
| yyparse.  |
`----------*/

int
yyparse (void)
{
    yy_state_fast_t yystate = 0;
    /* Number of tokens to shift before error messages enabled.  */
    int yyerrstatus = 0;

    /* Refer to the stacks through separate pointers, to allow yyoverflow
       to reallocate them elsewhere.  */

    /* Their size.  */
    YYPTRDIFF_T yystacksize = YYINITDEPTH;

    /* The state stack: array, bottom, top.  */
    yy_state_t yyssa[YYINITDEPTH];
    yy_state_t *yyss = yyssa;
    yy_state_t *yyssp = yyss;

    /* The semantic value stack: array, bottom, top.  */
    YYSTYPE yyvsa[YYINITDEPTH];
    YYSTYPE *yyvs = yyvsa;
    YYSTYPE *yyvsp = yyvs;

  int yyn;
  /* The return value of yyparse.  */
  int yyresult;
  /* Lookahead symbol kind.  */
  yysymbol_kind_t yytoken = YYSYMBOL_YYEMPTY;
  /* The variables used to return semantic value and location from the
     action routines.  */
  YYSTYPE yyval;



#define YYPOPSTACK(N)   (yyvsp -= (N), yyssp -= (N))

  /* The number of symbols on the RHS of the reduced rule.
     Keep to zero when no symbol should be popped.  */
  int yylen = 0;

  YYDPRINTF ((stderr, "Starting parse\n"));

  yychar = YYEMPTY; /* Cause a token to be read.  */

  goto yysetstate;


/*------------------------------------------------------------.
| yynewstate -- push a new state, which is found in yystate.  |
`------------------------------------------------------------*/
yynewstate:
  /* In all cases, when you get here, the value and location stacks
     have just been pushed.  So pushing a state here evens the stacks.  */
  yyssp++;


/*--------------------------------------------------------------------.
| yysetstate -- set current state (the top of the stack) to yystate.  |
`--------------------------------------------------------------------*/
yysetstate:
  YYDPRINTF ((stderr, "Entering state %d\n", yystate));
  YY_ASSERT (0 <= yystate && yystate < YYNSTATES);
  YY_IGNORE_USELESS_CAST_BEGIN
  *yyssp = YY_CAST (yy_state_t, yystate);
  YY_IGNORE_USELESS_CAST_END
  YY_STACK_PRINT (yyss, yyssp);

  if (yyss + yystacksize - 1 <= yyssp)
#if !defined yyoverflow && !defined YYSTACK_RELOCATE
    YYNOMEM;
#else
    {
      /* Get the current used size of the three stacks, in elements.  */
      YYPTRDIFF_T yysize = yyssp - yyss + 1;

# if defined yyoverflow
      {
        /* Give user a chance to reallocate the stack.  Use copies of
           these so that the &'s don't force the real ones into
           memory.  */
        yy_state_t *yyss1 = yyss;
        YYSTYPE *yyvs1 = yyvs;

        /* Each stack pointer address is followed by the size of the
           data in use in that stack, in bytes.  This used to be a
           conditional around just the two extra args, but that might
           be undefined if yyoverflow is a macro.  */
        yyoverflow (YY_("memory exhausted"),
                    &yyss1, yysize * YYSIZEOF (*yyssp),
                    &yyvs1, yysize * YYSIZEOF (*yyvsp),
                    &yystacksize);
        yyss = yyss1;
        yyvs = yyvs1;
      }
# else /* defined YYSTACK_RELOCATE */
      /* Extend the stack our own way.  */
      if (YYMAXDEPTH <= yystacksize)
        YYNOMEM;
      yystacksize *= 2;
      if (YYMAXDEPTH < yystacksize)
        yystacksize = YYMAXDEPTH;

      {
        yy_state_t *yyss1 = yyss;
        union yyalloc *yyptr =
          YY_CAST (union yyalloc *,
                   YYSTACK_ALLOC (YY_CAST (YYSIZE_T, YYSTACK_BYTES (yystacksize))));
        if (! yyptr)
          YYNOMEM;
        YYSTACK_RELOCATE (yyss_alloc, yyss);
        YYSTACK_RELOCATE (yyvs_alloc, yyvs);
#  undef YYSTACK_RELOCATE
        if (yyss1 != yyssa)
          YYSTACK_FREE (yyss1);
      }
# endif

      yyssp = yyss + yysize - 1;
      yyvsp = yyvs + yysize - 1;

      YY_IGNORE_USELESS_CAST_BEGIN
      YYDPRINTF ((stderr, "Stack size increased to %ld\n",
                  YY_CAST (long, yystacksize)));
      YY_IGNORE_USELESS_CAST_END

      if (yyss + yystacksize - 1 <= yyssp)
        YYABORT;
    }
#endif /* !defined yyoverflow && !defined YYSTACK_RELOCATE */


  if (yystate == YYFINAL)
    YYACCEPT;

  goto yybackup;


/*-----------.
| yybackup.  |
`-----------*/
yybackup:
  /* Do appropriate processing given the current state.  Read a
     lookahead token if we need one and don't already have one.  */

  /* First try to decide what to do without reference to lookahead token.  */
  yyn = yypact[yystate];
  if (yypact_value_is_default (yyn))
    goto yydefault;

  /* Not known => get a lookahead token if don't already have one.  */

  /* YYCHAR is either empty, or end-of-input, or a valid lookahead.  */
  if (yychar == YYEMPTY)
    {
      YYDPRINTF ((stderr, "Reading a token\n"));
      yychar = yylex ();
    }

  if (yychar <= YYEOF)
    {
      yychar = YYEOF;
      yytoken = YYSYMBOL_YYEOF;
      YYDPRINTF ((stderr, "Now at end of input.\n"));
    }
  else if (yychar == YYerror)
    {
      /* The scanner already issued an error message, process directly
         to error recovery.  But do not keep the error token as
         lookahead, it is too special and may lead us to an endless
         loop in error recovery. */
      yychar = YYUNDEF;
      yytoken = YYSYMBOL_YYerror;
      goto yyerrlab1;
    }
  else
    {
      yytoken = YYTRANSLATE (yychar);
      YY_SYMBOL_PRINT ("Next token is", yytoken, &yylval, &yylloc);
    }

  /* If the proper action on seeing token YYTOKEN is to reduce or to
     detect an error, take that action.  */
  yyn += yytoken;
  if (yyn < 0 || YYLAST < yyn || yycheck[yyn] != yytoken)
    goto yydefault;
  yyn = yytable[yyn];
  if (yyn <= 0)
    {
      if (yytable_value_is_error (yyn))
        goto yyerrlab;
      yyn = -yyn;
      goto yyreduce;
    }

  /* Count tokens shifted since error; after three, turn off error
     status.  */
  if (yyerrstatus)
    yyerrstatus--;

  /* Shift the lookahead token.  */
  YY_SYMBOL_PRINT ("Shifting", yytoken, &yylval, &yylloc);
  yystate = yyn;
  YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN
  *++yyvsp = yylval;
  YY_IGNORE_MAYBE_UNINITIALIZED_END

  /* Discard the shifted token.  */
  yychar = YYEMPTY;
  goto yynewstate;


/*-----------------------------------------------------------.
| yydefault -- do the default action for the current state.  |
`-----------------------------------------------------------*/
yydefault:
  yyn = yydefact[yystate];
  if (yyn == 0)
    goto yyerrlab;
  goto yyreduce;


/*-----------------------------.
| yyreduce -- do a reduction.  |
`-----------------------------*/
yyreduce:
  /* yyn is the number of a rule to reduce with.  */
  yylen = yyr2[yyn];

  /* If YYLEN is nonzero, implement the default value of the action:
     '$$ = $1'.

     Otherwise, the following line sets YYVAL to garbage.
     This behavior is undocumented and Bison
     users should not rely upon it.  Assigning to YYVAL
     unconditionally makes the parser a bit smaller, and it avoids a
     GCC warning that YYVAL may be used uninitialized.  */
  yyval = yyvsp[1-yylen];


  YY_REDUCE_PRINT (yyn);
  switch (yyn)
    {
  case 11: /* Statement: RoutineDecl sySemi  */
#line 183 "parser.y"
{
    statement_t *st = stAlloc();

    st->stKind = skRoutine;
    st->stRoutine = (yyvsp[-1].routine);
    rtCheckRoutine((yyvsp[-1].routine));
    if (BeVerbose)
	rtPrintRoutine((yyvsp[-1].routine));
}
#line 1553 "parser.c"
    break;

  case 12: /* Statement: sySkip sySemi  */
#line 193 "parser.y"
                                { rtSkip(1); }
#line 1559 "parser.c"
    break;

  case 13: /* Statement: sySkip syNumber sySemi  */
#line 195 "parser.y"
                                { rtSkip((yyvsp[-1].number)); }
#line 1565 "parser.c"
    break;

  case 17: /* Statement: error sySemi  */
#line 200 "parser.y"
                                { yyerrok; }
#line 1571 "parser.c"
    break;

  case 18: /* Subsystem: SubsystemStart SubsystemMods SubsystemName SubsystemBase  */
#line 205 "parser.y"
{
    if (BeVerbose)
    {
	printf("Subsystem %s: base = %u%s%s\n\n",
	       SubsystemName, SubsystemBase,
	       IsKernelUser ? ", KernelUser" : "",
	       IsKernelServer ? ", KernelServer" : "");
    }
    init_type();
}
#line 1586 "parser.c"
    break;

  case 19: /* SubsystemStart: sySubsystem  */
#line 218 "parser.y"
{
    if (SubsystemName != strNULL)
    {
	warn("previous Subsystem decl (of %s) will be ignored", SubsystemName);
	IsKernelUser = false;
	IsKernelServer = false;
	strfree((string_t) SubsystemName);
    }
}
#line 1600 "parser.c"
    break;

  case 22: /* SubsystemMod: syKernelUser  */
#line 234 "parser.y"
{
    if (IsKernelUser)
	warn("duplicate KernelUser keyword");
    IsKernelUser = true;
}
#line 1610 "parser.c"
    break;

  case 23: /* SubsystemMod: syKernelServer  */
#line 240 "parser.y"
{
    if (IsKernelServer)
	warn("duplicate KernelServer keyword");
    IsKernelServer = true;
}
#line 1620 "parser.c"
    break;

  case 24: /* SubsystemName: syIdentifier  */
#line 247 "parser.y"
                                                { SubsystemName = (yyvsp[0].identifier); }
#line 1626 "parser.c"
    break;

  case 25: /* SubsystemBase: syNumber  */
#line 250 "parser.y"
                                                { SubsystemBase = (yyvsp[0].number); }
#line 1632 "parser.c"
    break;

  case 26: /* MsgOption: LookString syMsgOption syString  */
#line 254 "parser.y"
{
    if (streql((yyvsp[0].string), "MACH_MSG_OPTION_NONE"))
    {
	MsgOption = strNULL;
	if (BeVerbose)
	    printf("MsgOption: canceled\n\n");
    }
    else
    {
	MsgOption = (yyvsp[0].string);
	if (BeVerbose)
	    printf("MsgOption %s\n\n",(yyvsp[0].string));
    }
}
#line 1651 "parser.c"
    break;

  case 27: /* WaitTime: LookString syWaitTime syString  */
#line 271 "parser.y"
{
    WaitTime = (yyvsp[0].string);
    if (BeVerbose)
	printf("WaitTime %s\n\n", WaitTime);
}
#line 1661 "parser.c"
    break;

  case 28: /* WaitTime: syNoWaitTime  */
#line 277 "parser.y"
{
    WaitTime = strNULL;
    if (BeVerbose)
	printf("NoWaitTime\n\n");
}
#line 1671 "parser.c"
    break;

  case 29: /* ServerPrefix: syServerPrefix syIdentifier  */
#line 285 "parser.y"
{
    ServerPrefix = (yyvsp[0].identifier);
    if (BeVerbose)
	printf("ServerPrefix %s\n\n", ServerPrefix);
}
#line 1681 "parser.c"
    break;

  case 30: /* UserPrefix: syUserPrefix syIdentifier  */
#line 293 "parser.y"
{
    UserPrefix = (yyvsp[0].identifier);
    if (BeVerbose)
	printf("UserPrefix %s\n\n", UserPrefix);
}
#line 1691 "parser.c"
    break;

  case 31: /* ServerDemux: syServerDemux syIdentifier  */
#line 301 "parser.y"
{
    ServerDemux = (yyvsp[0].identifier);
    if (BeVerbose)
	printf("ServerDemux %s\n\n", ServerDemux);
}
#line 1701 "parser.c"
    break;

  case 32: /* Import: LookFileName ImportIndicant syFileName  */
#line 309 "parser.y"
{
    statement_t *st = stAlloc();
    st->stKind = (yyvsp[-1].statement_kind);
    st->stFileName = (yyvsp[0].string);

    if (BeVerbose)
	printf("%s %s\n\n", import_name((yyvsp[-1].statement_kind)), (yyvsp[0].string));
}
#line 1714 "parser.c"
    break;

  case 33: /* ImportIndicant: syImport  */
#line 319 "parser.y"
                                                { (yyval.statement_kind) = skImport; }
#line 1720 "parser.c"
    break;

  case 34: /* ImportIndicant: syUImport  */
#line 320 "parser.y"
                                                { (yyval.statement_kind) = skUImport; }
#line 1726 "parser.c"
    break;

  case 35: /* ImportIndicant: sySImport  */
#line 321 "parser.y"
                                                { (yyval.statement_kind) = skSImport; }
#line 1732 "parser.c"
    break;

  case 36: /* RCSDecl: LookQString syRCSId syQString  */
#line 325 "parser.y"
{
    if (RCSId != strNULL)
	warn("previous RCS decl will be ignored");
    if (BeVerbose)
	printf("RCSId %s\n\n", (yyvsp[0].string));
    RCSId = (yyvsp[0].string);
}
#line 1744 "parser.c"
    break;

  case 37: /* TypeDecl: syType NamedTypeSpec  */
#line 335 "parser.y"
{
    identifier_t name = (yyvsp[0].type)->itName;

    if (itLookUp(name) != itNULL)
		error("overriding previous definition of %s", name);
    itInsert(name, (yyvsp[0].type));
}
#line 1756 "parser.c"
    break;

  case 38: /* NamedTypeSpec: syIdentifier syEqual TransTypeSpec  */
#line 345 "parser.y"
                                { itTypeDecl((yyvsp[-2].identifier), (yyval.type) = (yyvsp[0].type)); }
#line 1762 "parser.c"
    break;

  case 39: /* TransTypeSpec: TypeSpec  */
#line 349 "parser.y"
                                { (yyval.type) = itResetType((yyvsp[0].type)); }
#line 1768 "parser.c"
    break;

  case 40: /* TransTypeSpec: TransTypeSpec syInTran syColon syIdentifier syIdentifier syLParen syIdentifier syRParen  */
#line 352 "parser.y"
{
    (yyval.type) = (yyvsp[-7].type);

    if (((yyval.type)->itTransType != strNULL) && !streql((yyval.type)->itTransType, (yyvsp[-4].identifier)))
	warn("conflicting translation types (%s, %s)",
	     (yyval.type)->itTransType, (yyvsp[-4].identifier));
    (yyval.type)->itTransType = (yyvsp[-4].identifier);

    if (((yyval.type)->itInTrans != strNULL) && !streql((yyval.type)->itInTrans, (yyvsp[-3].identifier)))
	warn("conflicting in-translation functions (%s, %s)",
	     (yyval.type)->itInTrans, (yyvsp[-3].identifier));
    (yyval.type)->itInTrans = (yyvsp[-3].identifier);

    if (((yyval.type)->itServerType != strNULL) && !streql((yyval.type)->itServerType, (yyvsp[-1].identifier)))
	warn("conflicting server types (%s, %s)",
	     (yyval.type)->itServerType, (yyvsp[-1].identifier));
    (yyval.type)->itServerType = (yyvsp[-1].identifier);
}
#line 1791 "parser.c"
    break;

  case 41: /* TransTypeSpec: TransTypeSpec syInTranPayload syColon syIdentifier syIdentifier  */
#line 372 "parser.y"
{
    (yyval.type) = (yyvsp[-4].type);

    if (((yyval.type)->itTransType != strNULL) && !streql((yyval.type)->itTransType, (yyvsp[-1].identifier)))
	warn("conflicting translation types (%s, %s)",
	     (yyval.type)->itTransType, (yyvsp[-1].identifier));
    (yyval.type)->itTransType = (yyvsp[-1].identifier);

    if (((yyval.type)->itInTransPayload != strNULL) && !streql((yyval.type)->itInTransPayload, (yyvsp[0].identifier)))
	warn("conflicting in-translation functions (%s, %s)",
	     (yyval.type)->itInTransPayload, (yyvsp[0].identifier));
    (yyval.type)->itInTransPayload = (yyvsp[0].identifier);
}
#line 1809 "parser.c"
    break;

  case 42: /* TransTypeSpec: TransTypeSpec syOutTran syColon syIdentifier syIdentifier syLParen syIdentifier syRParen  */
#line 387 "parser.y"
{
    (yyval.type) = (yyvsp[-7].type);

    if (((yyval.type)->itServerType != strNULL) && !streql((yyval.type)->itServerType, (yyvsp[-4].identifier)))
	warn("conflicting server types (%s, %s)",
	     (yyval.type)->itServerType, (yyvsp[-4].identifier));
    (yyval.type)->itServerType = (yyvsp[-4].identifier);

    if (((yyval.type)->itOutTrans != strNULL) && !streql((yyval.type)->itOutTrans, (yyvsp[-3].identifier)))
	warn("conflicting out-translation functions (%s, %s)",
	     (yyval.type)->itOutTrans, (yyvsp[-3].identifier));
    (yyval.type)->itOutTrans = (yyvsp[-3].identifier);

    if (((yyval.type)->itTransType != strNULL) && !streql((yyval.type)->itTransType, (yyvsp[-1].identifier)))
	warn("conflicting translation types (%s, %s)",
	     (yyval.type)->itTransType, (yyvsp[-1].identifier));
    (yyval.type)->itTransType = (yyvsp[-1].identifier);
}
#line 1832 "parser.c"
    break;

  case 43: /* TransTypeSpec: TransTypeSpec syDestructor syColon syIdentifier syLParen syIdentifier syRParen  */
#line 407 "parser.y"
{
    (yyval.type) = (yyvsp[-6].type);

    if (((yyval.type)->itDestructor != strNULL) && !streql((yyval.type)->itDestructor, (yyvsp[-3].identifier)))
	warn("conflicting destructor functions (%s, %s)",
	     (yyval.type)->itDestructor, (yyvsp[-3].identifier));
    (yyval.type)->itDestructor = (yyvsp[-3].identifier);

    if (((yyval.type)->itTransType != strNULL) && !streql((yyval.type)->itTransType, (yyvsp[-1].identifier)))
	warn("conflicting translation types (%s, %s)",
	     (yyval.type)->itTransType, (yyvsp[-1].identifier));
    (yyval.type)->itTransType = (yyvsp[-1].identifier);
}
#line 1850 "parser.c"
    break;

  case 44: /* TransTypeSpec: TransTypeSpec syCType syColon syIdentifier  */
#line 421 "parser.y"
{
    (yyval.type) = (yyvsp[-3].type);

    if (((yyval.type)->itUserType != strNULL) && !streql((yyval.type)->itUserType, (yyvsp[0].identifier)))
	warn("conflicting user types (%s, %s)",
	     (yyval.type)->itUserType, (yyvsp[0].identifier));
    (yyval.type)->itUserType = (yyvsp[0].identifier);

    if (((yyval.type)->itServerType != strNULL) && !streql((yyval.type)->itServerType, (yyvsp[0].identifier)))
	warn("conflicting server types (%s, %s)",
	     (yyval.type)->itServerType, (yyvsp[0].identifier));
    (yyval.type)->itServerType = (yyvsp[0].identifier);
}
#line 1868 "parser.c"
    break;

  case 45: /* TransTypeSpec: TransTypeSpec syCUserType syColon syIdentifier  */
#line 435 "parser.y"
{
    (yyval.type) = (yyvsp[-3].type);

    if (((yyval.type)->itUserType != strNULL) && !streql((yyval.type)->itUserType, (yyvsp[0].identifier)))
	warn("conflicting user types (%s, %s)",
	     (yyval.type)->itUserType, (yyvsp[0].identifier));
    (yyval.type)->itUserType = (yyvsp[0].identifier);
}
#line 1881 "parser.c"
    break;

  case 46: /* TransTypeSpec: TransTypeSpec syCServerType syColon syIdentifier  */
#line 445 "parser.y"
{
    (yyval.type) = (yyvsp[-3].type);

    if (((yyval.type)->itServerType != strNULL) && !streql((yyval.type)->itServerType, (yyvsp[0].identifier)))
	warn("conflicting server types (%s, %s)",
	     (yyval.type)->itServerType, (yyvsp[0].identifier));
    (yyval.type)->itServerType = (yyvsp[0].identifier);
}
#line 1894 "parser.c"
    break;

  case 47: /* TypeSpec: BasicTypeSpec  */
#line 456 "parser.y"
                                { (yyval.type) = (yyvsp[0].type); }
#line 1900 "parser.c"
    break;

  case 48: /* TypeSpec: PrevTypeSpec  */
#line 458 "parser.y"
                                { (yyval.type) = (yyvsp[0].type); }
#line 1906 "parser.c"
    break;

  case 49: /* TypeSpec: VarArrayHead TypeSpec  */
#line 460 "parser.y"
                                { (yyval.type) = itVarArrayDecl((yyvsp[-1].number), (yyvsp[0].type)); }
#line 1912 "parser.c"
    break;

  case 50: /* TypeSpec: ArrayHead TypeSpec  */
#line 462 "parser.y"
                                { (yyval.type) = itArrayDecl((yyvsp[-1].number), (yyvsp[0].type)); }
#line 1918 "parser.c"
    break;

  case 51: /* TypeSpec: syCaret TypeSpec  */
#line 464 "parser.y"
                                { (yyval.type) = itPtrDecl((yyvsp[0].type)); }
#line 1924 "parser.c"
    break;

  case 52: /* TypeSpec: StructHead TypeSpec  */
#line 466 "parser.y"
                                { (yyval.type) = itStructArrayDecl((yyvsp[-1].number), (yyvsp[0].type)); }
#line 1930 "parser.c"
    break;

  case 53: /* TypeSpec: syStruct syLCBrack StructList syRCBrack  */
#line 468 "parser.y"
                                { (yyval.type) = itStructDecl((yyvsp[-1].structured_type).size_in_bytes, (yyvsp[-1].structured_type).type_alignment_in_bytes); }
#line 1936 "parser.c"
    break;

  case 54: /* TypeSpec: CStringSpec  */
#line 470 "parser.y"
                                { (yyval.type) = (yyvsp[0].type); }
#line 1942 "parser.c"
    break;

  case 55: /* StructList: syIdentifier syIdentifier sySemi  */
#line 474 "parser.y"
{
    ipc_type_t *t = itPrevDecl((yyvsp[-2].identifier));
    if (!t) {
        error("Type %s not found\n", (yyvsp[-2].identifier));
    }
    if (!t->itInLine) {
        error("Type %s must be inline\n", (yyvsp[-1].identifier));
    }

    (yyval.structured_type).type_alignment_in_bytes = t->itAlignment;
    (yyval.structured_type).size_in_bytes = t->itTypeSize;
}
#line 1959 "parser.c"
    break;

  case 56: /* StructList: StructList syIdentifier syIdentifier sySemi  */
#line 487 "parser.y"
{
    ipc_type_t *t = itPrevDecl((yyvsp[-2].identifier));
    if (!t) {
        error("Type %s not found\n", (yyvsp[-2].identifier));
    }
    if (!t->itInLine) {
        error("Type %s must be inline\n", (yyvsp[-2].identifier));
    }
    (yyval.structured_type).type_alignment_in_bytes = MAX(t->itAlignment, (yyvsp[-3].structured_type).type_alignment_in_bytes);
    int padding_bytes = 0;
    if ((yyvsp[-3].structured_type).size_in_bytes % t->itAlignment)
        padding_bytes = t->itAlignment - ((yyvsp[-3].structured_type).size_in_bytes % t->itAlignment);
    (yyval.structured_type).size_in_bytes = (yyvsp[-3].structured_type).size_in_bytes + padding_bytes + t->itTypeSize;
}
#line 1978 "parser.c"
    break;

  case 57: /* BasicTypeSpec: IPCType  */
#line 504 "parser.y"
{
    (yyval.type) = itShortDecl((yyvsp[0].symtype).innumber, (yyvsp[0].symtype).instr,
		     (yyvsp[0].symtype).outnumber, (yyvsp[0].symtype).outstr,
		     (yyvsp[0].symtype).size);
}
#line 1988 "parser.c"
    break;

  case 58: /* BasicTypeSpec: syLParen IPCType syComma IntExp IPCFlags syRParen  */
#line 511 "parser.y"
{
    (yyval.type) = itLongDecl((yyvsp[-4].symtype).innumber, (yyvsp[-4].symtype).instr,
		    (yyvsp[-4].symtype).outnumber, (yyvsp[-4].symtype).outstr,
		    (yyvsp[-4].symtype).size, (yyvsp[-2].number), (yyvsp[-1].flag));
}
#line 1998 "parser.c"
    break;

  case 59: /* IPCFlags: %empty  */
#line 519 "parser.y"
                                { (yyval.flag) = flNone; }
#line 2004 "parser.c"
    break;

  case 60: /* IPCFlags: IPCFlags syComma syIPCFlag  */
#line 521 "parser.y"
{
    if ((yyvsp[-2].flag) & (yyvsp[0].flag))
	warn("redundant IPC flag ignored");
    else
	(yyval.flag) = (yyvsp[-2].flag) | (yyvsp[0].flag);
}
#line 2015 "parser.c"
    break;

  case 61: /* IPCFlags: IPCFlags syComma syIPCFlag syLBrack syRBrack  */
#line 528 "parser.y"
{
    if ((yyvsp[-2].flag) != flDealloc)
	warn("only Dealloc is variable");
    else
	(yyval.flag) = (yyvsp[-4].flag) | flMaybeDealloc;
}
#line 2026 "parser.c"
    break;

  case 62: /* PrimIPCType: syNumber  */
#line 537 "parser.y"
{
    (yyval.symtype).innumber = (yyval.symtype).outnumber = (yyvsp[0].number);
    (yyval.symtype).instr = (yyval.symtype).outstr = strNULL;
    (yyval.symtype).size = 0;
}
#line 2036 "parser.c"
    break;

  case 63: /* PrimIPCType: sySymbolicType  */
#line 543 "parser.y"
                                { (yyval.symtype) = (yyvsp[0].symtype); }
#line 2042 "parser.c"
    break;

  case 64: /* IPCType: PrimIPCType  */
#line 547 "parser.y"
                                { (yyval.symtype) = (yyvsp[0].symtype); }
#line 2048 "parser.c"
    break;

  case 65: /* IPCType: PrimIPCType syBar PrimIPCType  */
#line 549 "parser.y"
{
    if ((yyvsp[-2].symtype).size != (yyvsp[0].symtype).size)
    {
	if ((yyvsp[-2].symtype).size == 0)
	    (yyval.symtype).size = (yyvsp[0].symtype).size;
	else if ((yyvsp[0].symtype).size == 0)
	    (yyval.symtype).size = (yyvsp[-2].symtype).size;
	else
	{
	    error("sizes in IPCTypes (%s %s %d, %s %s %d) aren't equal",
		  (yyvsp[-2].symtype).instr, (yyvsp[-2].symtype).outstr, (yyvsp[-2].symtype).size, (yyvsp[0].symtype).instr, (yyvsp[0].symtype).outstr, (yyvsp[0].symtype).size);
	    (yyval.symtype).size = 0;
	}
    }
    else
	(yyval.symtype).size = (yyvsp[-2].symtype).size;
    (yyval.symtype).innumber = (yyvsp[-2].symtype).innumber;
    (yyval.symtype).instr = (yyvsp[-2].symtype).instr;
    (yyval.symtype).outnumber = (yyvsp[0].symtype).outnumber;
    (yyval.symtype).outstr = (yyvsp[0].symtype).outstr;
}
#line 2074 "parser.c"
    break;

  case 66: /* PrevTypeSpec: syIdentifier  */
#line 573 "parser.y"
                                { (yyval.type) = itPrevDecl((yyvsp[0].identifier)); }
#line 2080 "parser.c"
    break;

  case 67: /* VarArrayHead: syArray syLBrack syRBrack syOf  */
#line 577 "parser.y"
                                { (yyval.number) = 0; }
#line 2086 "parser.c"
    break;

  case 68: /* VarArrayHead: syArray syLBrack syStar syRBrack syOf  */
#line 579 "parser.y"
                                { (yyval.number) = 0; }
#line 2092 "parser.c"
    break;

  case 69: /* VarArrayHead: syArray syLBrack syStar syColon IntExp syRBrack syOf  */
#line 582 "parser.y"
                                { (yyval.number) = (yyvsp[-2].number); }
#line 2098 "parser.c"
    break;

  case 70: /* ArrayHead: syArray syLBrack IntExp syRBrack syOf  */
#line 586 "parser.y"
                                { (yyval.number) = (yyvsp[-2].number); }
#line 2104 "parser.c"
    break;

  case 71: /* StructHead: syStruct syLBrack IntExp syRBrack syOf  */
#line 590 "parser.y"
                                { (yyval.number) = (yyvsp[-2].number); }
#line 2110 "parser.c"
    break;

  case 72: /* CStringSpec: syCString syLBrack IntExp syRBrack  */
#line 594 "parser.y"
                                { (yyval.type) = itCStringDecl((yyvsp[-1].number), false); }
#line 2116 "parser.c"
    break;

  case 73: /* CStringSpec: syCString syLBrack syStar syColon IntExp syRBrack  */
#line 597 "parser.y"
                                { (yyval.type) = itCStringDecl((yyvsp[-1].number), true); }
#line 2122 "parser.c"
    break;

  case 74: /* IntExp: IntExp syPlus IntExp  */
#line 601 "parser.y"
                                { (yyval.number) = (yyvsp[-2].number) + (yyvsp[0].number);	}
#line 2128 "parser.c"
    break;

  case 75: /* IntExp: IntExp syMinus IntExp  */
#line 603 "parser.y"
                                { (yyval.number) = (yyvsp[-2].number) - (yyvsp[0].number);	}
#line 2134 "parser.c"
    break;

  case 76: /* IntExp: IntExp syStar IntExp  */
#line 605 "parser.y"
                                { (yyval.number) = (yyvsp[-2].number) * (yyvsp[0].number);	}
#line 2140 "parser.c"
    break;

  case 77: /* IntExp: IntExp syDiv IntExp  */
#line 607 "parser.y"
                                { (yyval.number) = (yyvsp[-2].number) / (yyvsp[0].number);	}
#line 2146 "parser.c"
    break;

  case 78: /* IntExp: syNumber  */
#line 609 "parser.y"
                                { (yyval.number) = (yyvsp[0].number);	}
#line 2152 "parser.c"
    break;

  case 79: /* IntExp: syLParen IntExp syRParen  */
#line 611 "parser.y"
                                { (yyval.number) = (yyvsp[-1].number);	}
#line 2158 "parser.c"
    break;

  case 80: /* RoutineDecl: Routine  */
#line 615 "parser.y"
                                                        { (yyval.routine) = (yyvsp[0].routine); }
#line 2164 "parser.c"
    break;

  case 81: /* RoutineDecl: SimpleRoutine  */
#line 616 "parser.y"
                                                        { (yyval.routine) = (yyvsp[0].routine); }
#line 2170 "parser.c"
    break;

  case 82: /* Routine: syRoutine syIdentifier Arguments  */
#line 620 "parser.y"
                                { (yyval.routine) = rtMakeRoutine((yyvsp[-1].identifier), (yyvsp[0].argument)); }
#line 2176 "parser.c"
    break;

  case 83: /* SimpleRoutine: sySimpleRoutine syIdentifier Arguments  */
#line 624 "parser.y"
                                { (yyval.routine) = rtMakeSimpleRoutine((yyvsp[-1].identifier), (yyvsp[0].argument)); }
#line 2182 "parser.c"
    break;

  case 84: /* Arguments: syLParen syRParen  */
#line 628 "parser.y"
                                { (yyval.argument) = argNULL; }
#line 2188 "parser.c"
    break;

  case 85: /* Arguments: syLParen ArgumentList syRParen  */
#line 630 "parser.y"
                                { (yyval.argument) = (yyvsp[-1].argument); }
#line 2194 "parser.c"
    break;

  case 86: /* ArgumentList: Argument  */
#line 635 "parser.y"
                                { (yyval.argument) = (yyvsp[0].argument); }
#line 2200 "parser.c"
    break;

  case 87: /* ArgumentList: Argument sySemi ArgumentList  */
#line 637 "parser.y"
{
    (yyval.argument) = (yyvsp[-2].argument);
    (yyval.argument)->argNext = (yyvsp[0].argument);
}
#line 2209 "parser.c"
    break;

  case 88: /* Argument: Direction syIdentifier syColon ArgumentType IPCFlags  */
#line 644 "parser.y"
{
    (yyval.argument) = argAlloc();
    (yyval.argument)->argKind = (yyvsp[-4].direction);
    (yyval.argument)->argName = (yyvsp[-3].identifier);
    (yyval.argument)->argType = (yyvsp[-1].type);
    (yyval.argument)->argFlags = (yyvsp[0].flag);
}
#line 2221 "parser.c"
    break;

  case 89: /* Direction: %empty  */
#line 653 "parser.y"
                                                { (yyval.direction) = akNone; }
#line 2227 "parser.c"
    break;

  case 90: /* Direction: syIn  */
#line 654 "parser.y"
                                                { (yyval.direction) = akIn; }
#line 2233 "parser.c"
    break;

  case 91: /* Direction: syOut  */
#line 655 "parser.y"
                                                { (yyval.direction) = akOut; }
#line 2239 "parser.c"
    break;

  case 92: /* Direction: syInOut  */
#line 656 "parser.y"
                                                { (yyval.direction) = akInOut; }
#line 2245 "parser.c"
    break;

  case 93: /* Direction: syRequestPort  */
#line 657 "parser.y"
                                                { (yyval.direction) = akRequestPort; }
#line 2251 "parser.c"
    break;

  case 94: /* Direction: syReplyPort  */
#line 658 "parser.y"
                                                { (yyval.direction) = akReplyPort; }
#line 2257 "parser.c"
    break;

  case 95: /* Direction: sySReplyPort  */
#line 659 "parser.y"
                                                { (yyval.direction) = akSReplyPort; }
#line 2263 "parser.c"
    break;

  case 96: /* Direction: syUReplyPort  */
#line 660 "parser.y"
                                                { (yyval.direction) = akUReplyPort; }
#line 2269 "parser.c"
    break;

  case 97: /* Direction: syWaitTime  */
#line 661 "parser.y"
                                                { (yyval.direction) = akWaitTime; }
#line 2275 "parser.c"
    break;

  case 98: /* Direction: syMsgOption  */
#line 662 "parser.y"
                                                { (yyval.direction) = akMsgOption; }
#line 2281 "parser.c"
    break;

  case 99: /* Direction: syMsgSeqno  */
#line 663 "parser.y"
                                                { (yyval.direction) = akMsgSeqno; }
#line 2287 "parser.c"
    break;

  case 100: /* ArgumentType: syIdentifier  */
#line 667 "parser.y"
{
    (yyval.type) = itLookUp((yyvsp[0].identifier));
    if ((yyval.type) == itNULL)
	error("type '%s' not defined", (yyvsp[0].identifier));
}
#line 2297 "parser.c"
    break;

  case 101: /* ArgumentType: NamedTypeSpec  */
#line 673 "parser.y"
                                { (yyval.type) = (yyvsp[0].type); }
#line 2303 "parser.c"
    break;

  case 102: /* LookString: %empty  */
#line 677 "parser.y"
                                { LookString(); }
#line 2309 "parser.c"
    break;

  case 103: /* LookFileName: %empty  */
#line 681 "parser.y"
                                { LookFileName(); }
#line 2315 "parser.c"
    break;

  case 104: /* LookQString: %empty  */
#line 685 "parser.y"
                                { LookQString(); }
#line 2321 "parser.c"
    break;


#line 2325 "parser.c"

      default: break;
    }
  /* User semantic actions sometimes alter yychar, and that requires
     that yytoken be updated with the new translation.  We take the
     approach of translating immediately before every use of yytoken.
     One alternative is translating here after every semantic action,
     but that translation would be missed if the semantic action invokes
     YYABORT, YYACCEPT, or YYERROR immediately after altering yychar or
     if it invokes YYBACKUP.  In the case of YYABORT or YYACCEPT, an
     incorrect destructor might then be invoked immediately.  In the
     case of YYERROR or YYBACKUP, subsequent parser actions might lead
     to an incorrect destructor call or verbose syntax error message
     before the lookahead is translated.  */
  YY_SYMBOL_PRINT ("-> $$ =", YY_CAST (yysymbol_kind_t, yyr1[yyn]), &yyval, &yyloc);

  YYPOPSTACK (yylen);
  yylen = 0;

  *++yyvsp = yyval;

  /* Now 'shift' the result of the reduction.  Determine what state
     that goes to, based on the state we popped back to and the rule
     number reduced by.  */
  {
    const int yylhs = yyr1[yyn] - YYNTOKENS;
    const int yyi = yypgoto[yylhs] + *yyssp;
    yystate = (0 <= yyi && yyi <= YYLAST && yycheck[yyi] == *yyssp
               ? yytable[yyi]
               : yydefgoto[yylhs]);
  }

  goto yynewstate;


/*--------------------------------------.
| yyerrlab -- here on detecting error.  |
`--------------------------------------*/
yyerrlab:
  /* Make sure we have latest lookahead translation.  See comments at
     user semantic actions for why this is necessary.  */
  yytoken = yychar == YYEMPTY ? YYSYMBOL_YYEMPTY : YYTRANSLATE (yychar);
  /* If not already recovering from an error, report this error.  */
  if (!yyerrstatus)
    {
      ++yynerrs;
      yyerror (YY_("syntax error"));
    }

  if (yyerrstatus == 3)
    {
      /* If just tried and failed to reuse lookahead token after an
         error, discard it.  */

      if (yychar <= YYEOF)
        {
          /* Return failure if at end of input.  */
          if (yychar == YYEOF)
            YYABORT;
        }
      else
        {
          yydestruct ("Error: discarding",
                      yytoken, &yylval);
          yychar = YYEMPTY;
        }
    }

  /* Else will try to reuse lookahead token after shifting the error
     token.  */
  goto yyerrlab1;


/*---------------------------------------------------.
| yyerrorlab -- error raised explicitly by YYERROR.  |
`---------------------------------------------------*/
yyerrorlab:
  /* Pacify compilers when the user code never invokes YYERROR and the
     label yyerrorlab therefore never appears in user code.  */
  if (0)
    YYERROR;
  ++yynerrs;

  /* Do not reclaim the symbols of the rule whose action triggered
     this YYERROR.  */
  YYPOPSTACK (yylen);
  yylen = 0;
  YY_STACK_PRINT (yyss, yyssp);
  yystate = *yyssp;
  goto yyerrlab1;


/*-------------------------------------------------------------.
| yyerrlab1 -- common code for both syntax error and YYERROR.  |
`-------------------------------------------------------------*/
yyerrlab1:
  yyerrstatus = 3;      /* Each real token shifted decrements this.  */

  /* Pop stack until we find a state that shifts the error token.  */
  for (;;)
    {
      yyn = yypact[yystate];
      if (!yypact_value_is_default (yyn))
        {
          yyn += YYSYMBOL_YYerror;
          if (0 <= yyn && yyn <= YYLAST && yycheck[yyn] == YYSYMBOL_YYerror)
            {
              yyn = yytable[yyn];
              if (0 < yyn)
                break;
            }
        }

      /* Pop the current state because it cannot handle the error token.  */
      if (yyssp == yyss)
        YYABORT;


      yydestruct ("Error: popping",
                  YY_ACCESSING_SYMBOL (yystate), yyvsp);
      YYPOPSTACK (1);
      yystate = *yyssp;
      YY_STACK_PRINT (yyss, yyssp);
    }

  YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN
  *++yyvsp = yylval;
  YY_IGNORE_MAYBE_UNINITIALIZED_END


  /* Shift the error token.  */
  YY_SYMBOL_PRINT ("Shifting", YY_ACCESSING_SYMBOL (yyn), yyvsp, yylsp);

  yystate = yyn;
  goto yynewstate;


/*-------------------------------------.
| yyacceptlab -- YYACCEPT comes here.  |
`-------------------------------------*/
yyacceptlab:
  yyresult = 0;
  goto yyreturnlab;


/*-----------------------------------.
| yyabortlab -- YYABORT comes here.  |
`-----------------------------------*/
yyabortlab:
  yyresult = 1;
  goto yyreturnlab;


/*-----------------------------------------------------------.
| yyexhaustedlab -- YYNOMEM (memory exhaustion) comes here.  |
`-----------------------------------------------------------*/
yyexhaustedlab:
  yyerror (YY_("memory exhausted"));
  yyresult = 2;
  goto yyreturnlab;


/*----------------------------------------------------------.
| yyreturnlab -- parsing is finished, clean up and return.  |
`----------------------------------------------------------*/
yyreturnlab:
  if (yychar != YYEMPTY)
    {
      /* Make sure we have latest lookahead translation.  See comments at
         user semantic actions for why this is necessary.  */
      yytoken = YYTRANSLATE (yychar);
      yydestruct ("Cleanup: discarding lookahead",
                  yytoken, &yylval);
    }
  /* Do not reclaim the symbols of the rule whose action triggered
     this YYABORT or YYACCEPT.  */
  YYPOPSTACK (yylen);
  YY_STACK_PRINT (yyss, yyssp);
  while (yyssp != yyss)
    {
      yydestruct ("Cleanup: popping",
                  YY_ACCESSING_SYMBOL (+*yyssp), yyvsp);
      YYPOPSTACK (1);
    }
#ifndef yyoverflow
  if (yyss != yyssa)
    YYSTACK_FREE (yyss);
#endif

  return yyresult;
}

#line 688 "parser.y"


static const char *
import_name(statement_kind_t sk)
{
    switch (sk)
    {
      case skImport:
	return "Import";
      case skSImport:
	return "SImport";
      case skUImport:
	return "UImport";
      default:
	fatal("import_name(%d): not import statement", (int) sk);
	/*NOTREACHED*/
    }
}
