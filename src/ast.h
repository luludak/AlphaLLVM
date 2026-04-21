#pragma once
#include <string>
#include <vector>
#include <memory>
#include <variant>

enum class NodeKind {
    // Statements
    Program, Block, IfStmt, WhileStmt, ForStmt, ReturnStmt,
    BreakStmt, ContinueStmt, ExprStmt, FuncDef, LambdaDef,

    // Expressions
    AssignExpr, BinOp, UnaryOp, PostfixOp,
    CallExpr, IndexExpr, MemberExpr,
    IdExpr, IntLit, FloatLit, StrLit, BoolLit, NilLit,
    TableConstructor, TableElem, TableIndexElem,
    LocalDecl, FormalParam,
    // Special
    ArgList, ElemList
};

enum class BinOpKind { Add, Sub, Mul, Div, Mod, Eq, Neq, Lt, Gt, Leq, Geq, And, Or, Concat };
enum class UnaryOpKind { Minus, Not, PreInc, PreDec };
enum class PostfixOpKind { PostInc, PostDec };

struct ASTNode {
    NodeKind kind;
    int line = 0;

    // Value fields
    std::string sval;
    int ival = 0;
    double fval = 0;
    bool bval = false;
    BinOpKind binop{};
    UnaryOpKind unop{};
    PostfixOpKind postop{};
    bool isLocal = false;   // for id refs (::x vs local x)
    bool isGlobal = false;

    std::vector<std::unique_ptr<ASTNode>> children;

    ASTNode(NodeKind k, int ln = 0) : kind(k), line(ln) {}
    // Non-copyable, movable
    ASTNode(const ASTNode&) = delete;
    ASTNode& operator=(const ASTNode&) = delete;
};

using ASTNodePtr = std::unique_ptr<ASTNode>;

// Helper factories
inline ASTNodePtr makeNode(NodeKind k, int line = 0) {
    return std::make_unique<ASTNode>(k, line);
}
