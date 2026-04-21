%{
#include "ast.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Bison 2.3 generates the union header before seeing our C++ types.
   We store ASTNode* as void* and cast with these macros. */
#define AN(x)  ((ASTNode*)(x))
#define VN(x)  ((void*)(x))

extern int yylineno;
extern "C" int yylex();
void yyerror(const char* msg);

ASTNodePtr g_root;
%}

%union {
    int      ival;
    double   fval;
    char*    sval;
    int      bval;
    void*    node;
}

%token <sval> ID STRLIT
%token <ival> INTLIT
%token <fval> FLOATLIT
%token <bval> TRUE FALSE
%token IF ELSE WHILE FOR FUNCTION RETURN BREAK CONTINUE
%token AND OR NOT NIL LOCAL LAMBDA
%token EQ NEQ GEQ LEQ GT LT ASSIGN
%token PLUSPLUS MINUSMINUS PLUS MINUS MUL DIV MOD
%token LBRACE RBRACE LBRACKET RBRACKET LPAREN RPAREN
%token SEMICOLON COMMA COLON DCOLON DOT DOTDOT

%type <node> program stmtlist stmt
%type <node> ifstmt whilestmt forstmt returnstmt breakstmt continuestmt
%type <node> funcdef lambdadef formallist
%type <node> expr assignexpr callexpr memberexpr primary
%type <node> tableconstructor elemlist elem
%type <node> arglist argsopt lvalue
%type <node> block

%right ASSIGN
%left OR
%left AND
%nonassoc EQ NEQ
%nonassoc LT GT LEQ GEQ
%right DOTDOT
%left PLUS MINUS
%left MUL DIV MOD
%right NOT UMINUS
%left DOT LBRACKET
%left LPAREN

%start program

%%

program
    : stmtlist
        { auto n = makeNode(NodeKind::Program, 1);
          n->children.push_back(ASTNodePtr(AN($1)));
          g_root = std::move(n); }
    ;

stmtlist
    : stmtlist stmt
        { AN($1)->children.push_back(ASTNodePtr(AN($2))); $$ = $1; }
    | /* empty */
        { $$ = VN(makeNode(NodeKind::Block, yylineno).release()); }
    ;

stmt
    : expr SEMICOLON
        { auto n = makeNode(NodeKind::ExprStmt, yylineno);
          n->children.push_back(ASTNodePtr(AN($1)));
          $$ = VN(n.release()); }
    | LOCAL ID SEMICOLON
        { auto n = makeNode(NodeKind::LocalDecl, yylineno);
          n->sval = $2; free($2);
          $$ = VN(n.release()); }
    | LOCAL ID ASSIGN expr SEMICOLON
        { auto n = makeNode(NodeKind::LocalDecl, yylineno);
          n->sval = $2; free($2);
          n->children.push_back(ASTNodePtr(AN($4)));
          $$ = VN(n.release()); }
    | ifstmt       { $$ = $1; }
    | whilestmt    { $$ = $1; }
    | forstmt      { $$ = $1; }
    | returnstmt   { $$ = $1; }
    | breakstmt    { $$ = $1; }
    | continuestmt { $$ = $1; }
    | funcdef      { $$ = $1; }
    | block        { $$ = $1; }
    | SEMICOLON
        { $$ = VN(makeNode(NodeKind::Block, yylineno).release()); }
    ;

block
    : LBRACE stmtlist RBRACE
        { AN($2)->kind = NodeKind::Block; $$ = $2; }
    ;

ifstmt
    : IF LPAREN expr RPAREN stmt
        { auto n = makeNode(NodeKind::IfStmt, yylineno);
          n->children.push_back(ASTNodePtr(AN($3)));
          n->children.push_back(ASTNodePtr(AN($5)));
          $$ = VN(n.release()); }
    | IF LPAREN expr RPAREN stmt ELSE stmt
        { auto n = makeNode(NodeKind::IfStmt, yylineno);
          n->children.push_back(ASTNodePtr(AN($3)));
          n->children.push_back(ASTNodePtr(AN($5)));
          n->children.push_back(ASTNodePtr(AN($7)));
          $$ = VN(n.release()); }
    ;

whilestmt
    : WHILE LPAREN expr RPAREN stmt
        { auto n = makeNode(NodeKind::WhileStmt, yylineno);
          n->children.push_back(ASTNodePtr(AN($3)));
          n->children.push_back(ASTNodePtr(AN($5)));
          $$ = VN(n.release()); }
    ;

forstmt
    : FOR LPAREN expr SEMICOLON expr SEMICOLON expr RPAREN stmt
        { auto n = makeNode(NodeKind::ForStmt, yylineno);
          n->children.push_back(ASTNodePtr(AN($3)));
          n->children.push_back(ASTNodePtr(AN($5)));
          n->children.push_back(ASTNodePtr(AN($7)));
          n->children.push_back(ASTNodePtr(AN($9)));
          $$ = VN(n.release()); }
    | FOR LPAREN SEMICOLON expr SEMICOLON expr RPAREN stmt
        { auto n = makeNode(NodeKind::ForStmt, yylineno);
          n->children.push_back(ASTNodePtr(nullptr));
          n->children.push_back(ASTNodePtr(AN($4)));
          n->children.push_back(ASTNodePtr(AN($6)));
          n->children.push_back(ASTNodePtr(AN($8)));
          $$ = VN(n.release()); }
    | FOR LPAREN expr SEMICOLON SEMICOLON expr RPAREN stmt
        { auto n = makeNode(NodeKind::ForStmt, yylineno);
          n->children.push_back(ASTNodePtr(AN($3)));
          n->children.push_back(ASTNodePtr(nullptr));
          n->children.push_back(ASTNodePtr(AN($6)));
          n->children.push_back(ASTNodePtr(AN($8)));
          $$ = VN(n.release()); }
    ;

returnstmt
    : RETURN expr SEMICOLON
        { auto n = makeNode(NodeKind::ReturnStmt, yylineno);
          n->children.push_back(ASTNodePtr(AN($2)));
          $$ = VN(n.release()); }
    | RETURN SEMICOLON
        { $$ = VN(makeNode(NodeKind::ReturnStmt, yylineno).release()); }
    ;

breakstmt
    : BREAK SEMICOLON
        { $$ = VN(makeNode(NodeKind::BreakStmt, yylineno).release()); }
    ;

continuestmt
    : CONTINUE SEMICOLON
        { $$ = VN(makeNode(NodeKind::ContinueStmt, yylineno).release()); }
    ;

funcdef
    : FUNCTION ID LPAREN formallist RPAREN block
        { auto n = makeNode(NodeKind::FuncDef, yylineno);
          n->sval = $2; free($2);
          n->children.push_back(ASTNodePtr(AN($4)));
          n->children.push_back(ASTNodePtr(AN($6)));
          $$ = VN(n.release()); }
    ;

lambdadef
    : LAMBDA LPAREN formallist RPAREN block
        { auto n = makeNode(NodeKind::LambdaDef, yylineno);
          n->children.push_back(ASTNodePtr(AN($3)));
          n->children.push_back(ASTNodePtr(AN($5)));
          $$ = VN(n.release()); }
    ;

formallist
    : formallist COMMA ID
        { auto p = makeNode(NodeKind::FormalParam, yylineno);
          p->sval = $3; free($3);
          AN($1)->children.push_back(std::move(p));
          $$ = $1; }
    | ID
        { auto list = makeNode(NodeKind::ElemList, yylineno);
          auto p    = makeNode(NodeKind::FormalParam, yylineno);
          p->sval = $1; free($1);
          list->children.push_back(std::move(p));
          $$ = VN(list.release()); }
    | /* empty */
        { $$ = VN(makeNode(NodeKind::ElemList, yylineno).release()); }
    ;

expr
    : assignexpr    { $$ = $1; }
    | expr PLUS expr
        { auto n = makeNode(NodeKind::BinOp, yylineno);
          n->binop = BinOpKind::Add;
          n->children.push_back(ASTNodePtr(AN($1)));
          n->children.push_back(ASTNodePtr(AN($3)));
          $$ = VN(n.release()); }
    | expr MINUS expr
        { auto n = makeNode(NodeKind::BinOp, yylineno);
          n->binop = BinOpKind::Sub;
          n->children.push_back(ASTNodePtr(AN($1)));
          n->children.push_back(ASTNodePtr(AN($3)));
          $$ = VN(n.release()); }
    | expr MUL expr
        { auto n = makeNode(NodeKind::BinOp, yylineno);
          n->binop = BinOpKind::Mul;
          n->children.push_back(ASTNodePtr(AN($1)));
          n->children.push_back(ASTNodePtr(AN($3)));
          $$ = VN(n.release()); }
    | expr DIV expr
        { auto n = makeNode(NodeKind::BinOp, yylineno);
          n->binop = BinOpKind::Div;
          n->children.push_back(ASTNodePtr(AN($1)));
          n->children.push_back(ASTNodePtr(AN($3)));
          $$ = VN(n.release()); }
    | expr MOD expr
        { auto n = makeNode(NodeKind::BinOp, yylineno);
          n->binop = BinOpKind::Mod;
          n->children.push_back(ASTNodePtr(AN($1)));
          n->children.push_back(ASTNodePtr(AN($3)));
          $$ = VN(n.release()); }
    | expr EQ expr
        { auto n = makeNode(NodeKind::BinOp, yylineno);
          n->binop = BinOpKind::Eq;
          n->children.push_back(ASTNodePtr(AN($1)));
          n->children.push_back(ASTNodePtr(AN($3)));
          $$ = VN(n.release()); }
    | expr NEQ expr
        { auto n = makeNode(NodeKind::BinOp, yylineno);
          n->binop = BinOpKind::Neq;
          n->children.push_back(ASTNodePtr(AN($1)));
          n->children.push_back(ASTNodePtr(AN($3)));
          $$ = VN(n.release()); }
    | expr LT expr
        { auto n = makeNode(NodeKind::BinOp, yylineno);
          n->binop = BinOpKind::Lt;
          n->children.push_back(ASTNodePtr(AN($1)));
          n->children.push_back(ASTNodePtr(AN($3)));
          $$ = VN(n.release()); }
    | expr GT expr
        { auto n = makeNode(NodeKind::BinOp, yylineno);
          n->binop = BinOpKind::Gt;
          n->children.push_back(ASTNodePtr(AN($1)));
          n->children.push_back(ASTNodePtr(AN($3)));
          $$ = VN(n.release()); }
    | expr LEQ expr
        { auto n = makeNode(NodeKind::BinOp, yylineno);
          n->binop = BinOpKind::Leq;
          n->children.push_back(ASTNodePtr(AN($1)));
          n->children.push_back(ASTNodePtr(AN($3)));
          $$ = VN(n.release()); }
    | expr GEQ expr
        { auto n = makeNode(NodeKind::BinOp, yylineno);
          n->binop = BinOpKind::Geq;
          n->children.push_back(ASTNodePtr(AN($1)));
          n->children.push_back(ASTNodePtr(AN($3)));
          $$ = VN(n.release()); }
    | expr AND expr
        { auto n = makeNode(NodeKind::BinOp, yylineno);
          n->binop = BinOpKind::And;
          n->children.push_back(ASTNodePtr(AN($1)));
          n->children.push_back(ASTNodePtr(AN($3)));
          $$ = VN(n.release()); }
    | expr OR expr
        { auto n = makeNode(NodeKind::BinOp, yylineno);
          n->binop = BinOpKind::Or;
          n->children.push_back(ASTNodePtr(AN($1)));
          n->children.push_back(ASTNodePtr(AN($3)));
          $$ = VN(n.release()); }
    | expr DOTDOT expr
        { auto n = makeNode(NodeKind::BinOp, yylineno);
          n->binop = BinOpKind::Concat;
          n->children.push_back(ASTNodePtr(AN($1)));
          n->children.push_back(ASTNodePtr(AN($3)));
          $$ = VN(n.release()); }
    | NOT expr
        { auto n = makeNode(NodeKind::UnaryOp, yylineno);
          n->unop = UnaryOpKind::Not;
          n->children.push_back(ASTNodePtr(AN($2)));
          $$ = VN(n.release()); }
    | MINUS expr %prec UMINUS
        { auto n = makeNode(NodeKind::UnaryOp, yylineno);
          n->unop = UnaryOpKind::Minus;
          n->children.push_back(ASTNodePtr(AN($2)));
          $$ = VN(n.release()); }
    | PLUSPLUS lvalue
        { auto n = makeNode(NodeKind::UnaryOp, yylineno);
          n->unop = UnaryOpKind::PreInc;
          n->children.push_back(ASTNodePtr(AN($2)));
          $$ = VN(n.release()); }
    | MINUSMINUS lvalue
        { auto n = makeNode(NodeKind::UnaryOp, yylineno);
          n->unop = UnaryOpKind::PreDec;
          n->children.push_back(ASTNodePtr(AN($2)));
          $$ = VN(n.release()); }
    | lvalue PLUSPLUS
        { auto n = makeNode(NodeKind::PostfixOp, yylineno);
          n->postop = PostfixOpKind::PostInc;
          n->children.push_back(ASTNodePtr(AN($1)));
          $$ = VN(n.release()); }
    | lvalue MINUSMINUS
        { auto n = makeNode(NodeKind::PostfixOp, yylineno);
          n->postop = PostfixOpKind::PostDec;
          n->children.push_back(ASTNodePtr(AN($1)));
          $$ = VN(n.release()); }
    | callexpr  { $$ = $1; }
    | primary   { $$ = $1; }
    ;

assignexpr
    : lvalue ASSIGN expr
        { auto n = makeNode(NodeKind::AssignExpr, yylineno);
          n->children.push_back(ASTNodePtr(AN($1)));
          n->children.push_back(ASTNodePtr(AN($3)));
          $$ = VN(n.release()); }
    ;

lvalue
    : ID
        { auto n = makeNode(NodeKind::IdExpr, yylineno);
          n->sval = $1; free($1);
          $$ = VN(n.release()); }
    | LOCAL ID
        { auto n = makeNode(NodeKind::IdExpr, yylineno);
          n->sval = $2; free($2); n->isLocal = true;
          $$ = VN(n.release()); }
    | DCOLON ID
        { auto n = makeNode(NodeKind::IdExpr, yylineno);
          n->sval = $2; free($2); n->isGlobal = true;
          $$ = VN(n.release()); }
    | memberexpr { $$ = $1; }
    ;

memberexpr
    : lvalue DOT ID
        { auto n = makeNode(NodeKind::MemberExpr, yylineno);
          n->sval = $3; free($3);
          n->children.push_back(ASTNodePtr(AN($1)));
          $$ = VN(n.release()); }
    | lvalue LBRACKET expr RBRACKET
        { auto n = makeNode(NodeKind::IndexExpr, yylineno);
          n->children.push_back(ASTNodePtr(AN($1)));
          n->children.push_back(ASTNodePtr(AN($3)));
          $$ = VN(n.release()); }
    | callexpr DOT ID
        { auto n = makeNode(NodeKind::MemberExpr, yylineno);
          n->sval = $3; free($3);
          n->children.push_back(ASTNodePtr(AN($1)));
          $$ = VN(n.release()); }
    | callexpr LBRACKET expr RBRACKET
        { auto n = makeNode(NodeKind::IndexExpr, yylineno);
          n->children.push_back(ASTNodePtr(AN($1)));
          n->children.push_back(ASTNodePtr(AN($3)));
          $$ = VN(n.release()); }
    ;

callexpr
    : lvalue LPAREN argsopt RPAREN
        { auto n = makeNode(NodeKind::CallExpr, yylineno);
          n->children.push_back(ASTNodePtr(AN($1)));
          n->children.push_back(ASTNodePtr(AN($3)));
          $$ = VN(n.release()); }
    | callexpr LPAREN argsopt RPAREN
        { auto n = makeNode(NodeKind::CallExpr, yylineno);
          n->children.push_back(ASTNodePtr(AN($1)));
          n->children.push_back(ASTNodePtr(AN($3)));
          $$ = VN(n.release()); }
    | LPAREN funcdef RPAREN LPAREN argsopt RPAREN
        { auto n = makeNode(NodeKind::CallExpr, yylineno);
          n->children.push_back(ASTNodePtr(AN($2)));
          n->children.push_back(ASTNodePtr(AN($5)));
          $$ = VN(n.release()); }
    | LPAREN lambdadef RPAREN LPAREN argsopt RPAREN
        { auto n = makeNode(NodeKind::CallExpr, yylineno);
          n->children.push_back(ASTNodePtr(AN($2)));
          n->children.push_back(ASTNodePtr(AN($5)));
          $$ = VN(n.release()); }
    ;

argsopt
    : arglist   { $$ = $1; }
    | /* empty */
        { $$ = VN(makeNode(NodeKind::ArgList, yylineno).release()); }
    ;

arglist
    : arglist COMMA expr
        { AN($1)->children.push_back(ASTNodePtr(AN($3))); $$ = $1; }
    | expr
        { auto n = makeNode(NodeKind::ArgList, yylineno);
          n->children.push_back(ASTNodePtr(AN($1)));
          $$ = VN(n.release()); }
    ;

primary
    : INTLIT
        { auto n = makeNode(NodeKind::IntLit, yylineno);
          n->ival = $1; $$ = VN(n.release()); }
    | FLOATLIT
        { auto n = makeNode(NodeKind::FloatLit, yylineno);
          n->fval = $1; $$ = VN(n.release()); }
    | STRLIT
        { auto n = makeNode(NodeKind::StrLit, yylineno);
          n->sval = $1; free($1); $$ = VN(n.release()); }
    | TRUE
        { auto n = makeNode(NodeKind::BoolLit, yylineno);
          n->bval = true; $$ = VN(n.release()); }
    | FALSE
        { auto n = makeNode(NodeKind::BoolLit, yylineno);
          n->bval = false; $$ = VN(n.release()); }
    | NIL
        { $$ = VN(makeNode(NodeKind::NilLit, yylineno).release()); }
    | LPAREN expr RPAREN { $$ = $2; }
    | tableconstructor   { $$ = $1; }
    | funcdef            { $$ = $1; }
    | lambdadef          { $$ = $1; }
    | lvalue             { $$ = $1; }
    ;

tableconstructor
    : LBRACKET elemlist RBRACKET
        { AN($2)->kind = NodeKind::TableConstructor; $$ = $2; }
    | LBRACKET RBRACKET
        { $$ = VN(makeNode(NodeKind::TableConstructor, yylineno).release()); }
    ;

elemlist
    : elemlist COMMA elem
        { AN($1)->children.push_back(ASTNodePtr(AN($3))); $$ = $1; }
    | elem
        { auto n = makeNode(NodeKind::ElemList, yylineno);
          n->children.push_back(ASTNodePtr(AN($1)));
          $$ = VN(n.release()); }
    ;

elem
    : expr  { $$ = $1; }
    | ID COLON expr
        { auto n = makeNode(NodeKind::TableElem, yylineno);
          n->sval = $1; free($1);
          n->children.push_back(ASTNodePtr(AN($3)));
          $$ = VN(n.release()); }
    | LBRACKET expr RBRACKET COLON expr
        { auto n = makeNode(NodeKind::TableIndexElem, yylineno);
          n->children.push_back(ASTNodePtr(AN($2)));
          n->children.push_back(ASTNodePtr(AN($5)));
          $$ = VN(n.release()); }
    ;

%%

void yyerror(const char* msg) {
    fprintf(stderr, "Parse error at line %d: %s\n", yylineno, msg);
}
