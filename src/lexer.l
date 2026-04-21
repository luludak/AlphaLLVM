%{
#include "parser.tab.h"
#include <string.h>
#include <stdlib.h>

static int comment_depth = 0;
%}

%option noyywrap
%option yylineno

%x BLOCK_COMMENT
%x LINE_COMMENT

ID          [a-zA-Z_][a-zA-Z0-9_]*
INTLIT      [0-9]+
FLOATLIT    [0-9]+\.[0-9]+
STRING      \"([^\"\\]|\\.)*\"
WS          [ \t\r]+

%%

"//"                    { BEGIN(LINE_COMMENT); }
<LINE_COMMENT>\n        { yylineno++; BEGIN(INITIAL); }
<LINE_COMMENT>.         {}

"/*"                    { comment_depth = 1; BEGIN(BLOCK_COMMENT); }
<BLOCK_COMMENT>"/*"     { comment_depth++; }
<BLOCK_COMMENT>"*/"     { if (--comment_depth == 0) BEGIN(INITIAL); }
<BLOCK_COMMENT>\n       { yylineno++; }
<BLOCK_COMMENT>.        {}

"if"                    { return IF; }
"else"                  { return ELSE; }
"while"                 { return WHILE; }
"for"                   { return FOR; }
"function"              { return FUNCTION; }
"return"                { return RETURN; }
"break"                 { return BREAK; }
"continue"              { return CONTINUE; }
"and"                   { return AND; }
"or"                    { return OR; }
"not"                   { return NOT; }
"true"                  { yylval.bval = 1; return TRUE; }
"false"                 { yylval.bval = 0; return FALSE; }
"nil"                   { return NIL; }
"local"                 { return LOCAL; }
"lambda"                { return LAMBDA; }

{INTLIT}                { yylval.ival = atoi(yytext); return INTLIT; }
{FLOATLIT}              { yylval.fval = atof(yytext); return FLOATLIT; }
{STRING}                {
                            /* strip quotes, process escape sequences, pure C */
                            int len = (int)strlen(yytext) - 2; /* without quotes */
                            char* buf = (char*)malloc(len + 1);
                            int j = 0;
                            for (int i = 1; i <= len; i++) {
                                if (yytext[i] == '\\' && i < len) {
                                    i++;
                                    switch (yytext[i]) {
                                        case 'n':  buf[j++] = '\n'; break;
                                        case 't':  buf[j++] = '\t'; break;
                                        case '"':  buf[j++] = '"';  break;
                                        case '\\': buf[j++] = '\\'; break;
                                        default:   buf[j++] = yytext[i]; break;
                                    }
                                } else {
                                    buf[j++] = yytext[i];
                                }
                            }
                            buf[j] = '\0';
                            yylval.sval = buf; /* caller owns this memory */
                            return STRLIT;
                        }
{ID}                    { yylval.sval = strdup(yytext); return ID; }

"=="                    { return EQ; }
"!="                    { return NEQ; }
">="                    { return GEQ; }
"<="                    { return LEQ; }
">"                     { return GT; }
"<"                     { return LT; }
"="                     { return ASSIGN; }
"++"                    { return PLUSPLUS; }
"--"                    { return MINUSMINUS; }
"+"                     { return PLUS; }
"-"                     { return MINUS; }
"*"                     { return MUL; }
"/"                     { return DIV; }
"%"                     { return MOD; }
"{"                     { return LBRACE; }
"}"                     { return RBRACE; }
"["                     { return LBRACKET; }
"]"                     { return RBRACKET; }
"("                     { return LPAREN; }
")"                     { return RPAREN; }
";"                     { return SEMICOLON; }
","                     { return COMMA; }
":"                     { return COLON; }
"::"                    { return DCOLON; }
"."                     { return DOT; }
".."                    { return DOTDOT; }

{WS}                    {}
\n                      { yylineno++; }

.                       { fprintf(stderr, "Lexer error: unknown char '%s' at line %d\n", yytext, yylineno); }

%%
