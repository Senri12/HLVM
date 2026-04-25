grammar SimpleLang;



options {

    language = C;

    output = AST;

}



tokens {
    PROGRAM;
    FUNC_DEF;
    FUNC_SIG;
    ARG_DEF;
    VAR_DECL;

    VAR_INIT;

    BLOCK;

    IF;

    ELSE;

    WHILE;

    DO_WHILE;

    BREAK;

    EXPR_STMT;

    ASSIGN;

    CALL;

    ARRAY_INDEX;

    UNARY;

    TYPE_REF;

    ARRAY_TYPE;

    ARG_LIST;
    EXPR_LIST;
    RETURN;     // AST-узел для return
    CONTINUE;   // AST-узел для continue
    CLASS_DEF;
    MEMBER;
    FIELD_DEF;
    MODIFIER;
    TEMPLATE_PARAMS;
    TYPE_ARGS;
    INHERITANCE;
    MEMBER_ACCESS;
    ARRAY_DECL;
    ASYNC;      // marker for async functions (Task 2 var.1)
    AWAIT;      // unary expression: await expr
}


// --- Лексика ---

BOOL    : 'true' | 'false';
CLASS   : 'class';
PUBLIC  : 'public';
PRIVATE : 'private';
ASYNC_KW : 'async';
AWAIT_KW : 'await';

HEX     : '0' ('x'|'X') ('0'..'9'|'a'..'f'|'A'..'F')+;

BITS    : '0' ('b'|'B') ('0'|'1')+;

DEC     : '0' | ('1'..'9' ('0'..'9')* );

STRING  : '"' ( ESC | ~('\\'|'"'|'\r'|'\n') )* '"';

CHAR    : '\'' ( ESC | ~('\\'|'\'') ) '\'';







ID      : ('a'..'z'|'A'..'Z'|'_') ('a'..'z'|'A'..'Z'|'0'..'9'|'_')*;



fragment ESC

    : '\\' ('b'|'t'|'n'|'f'|'r'|'\\'|'\''|'"')

    | '\\' ('0'..'3') ('0'..'7') ('0'..'7')

    | '\\' ('0'..'7') ('0'..'7')?

    ;



// Пробелы и комментарии

WS : (' ' | '\t' | '\r' | '\n')+ { $channel = HIDDEN; } ;

LINE_COMMENT  : '//' ~('\r' | '\n')* { $channel = HIDDEN; } ;

BLOCK_COMMENT : '/*' ( options {greedy=false;} : . )* '*/' { $channel = HIDDEN; } ;



// --- Парсер ---
source     : sourceItem* EOF -> ^(PROGRAM sourceItem*) ;
sourceItem
    : classDef
    | funcDef
    ;

classDef
    : CLASS ID templateParams? inheritanceSpec? '{' member* '}'
      -> ^(CLASS_DEF ID templateParams? inheritanceSpec? member*)
    ;

templateParams
    : '<' ID (',' ID)* '>'
      -> ^(TEMPLATE_PARAMS ID+)
    ;

inheritanceSpec
    : ':' typeRef
      -> ^(INHERITANCE typeRef)
    ;

member
    : modifier? (((ASYNC_KW? typeRef ID '(')=> typedMethodDef) | ((ASYNC_KW? ID '(')=> implicitMethodDef) | field)
      -> ^(MEMBER modifier? typedMethodDef? implicitMethodDef? field?)
    ;

field
    : typeRef varInit (',' varInit)* ';'
      -> ^(FIELD_DEF typeRef varInit+)
    ;

modifier
    : PUBLIC -> ^(MODIFIER PUBLIC)
    | PRIVATE -> ^(MODIFIER PRIVATE)
    ;


// === Functions ===

funcDef
    : typedMethodDef
    | implicitMethodDef
    ;

typedMethodDef
    : ASYNC_KW typeRef ID '(' argList ')' (block | ';')
      -> ^(FUNC_DEF ^(FUNC_SIG ^(ASYNC) typeRef ID argList) block?)
    | typeRef ID '(' argList ')' (block | ';')
      -> ^(FUNC_DEF ^(FUNC_SIG typeRef ID argList) block?)
    ;

implicitMethodDef
    : ASYNC_KW ID '(' argList ')' (block | ';')
      -> ^(FUNC_DEF ^(FUNC_SIG ^(ASYNC) ID argList) block?)
    | ID '(' argList ')' (block | ';')
      -> ^(FUNC_DEF ^(FUNC_SIG ID argList) block?)
    ;



funcSignature
    : typeRef ID '(' argList ')' 
      -> ^(FUNC_SIG typeRef ID argList)
    | ID '(' argList ')'
      -> ^(FUNC_SIG ID argList)
    ;



argList : (argDef (',' argDef)*)? -> ^(ARG_LIST argDef*) ;

argDef  : typeRef? ID -> ^(ARG_DEF typeRef? ID) ;



// === Types ===

typeRef
    : builtinType typeArgs? arrayDims? -> ^(TYPE_REF builtinType typeArgs? arrayDims?)
    | ID typeArgs? arrayDims?          -> ^(TYPE_REF ID typeArgs? arrayDims?)
    ;

typeArgs
    : '<' typeRef (',' typeRef)* '>'
      -> ^(TYPE_ARGS typeRef+)
    ;


builtinType

    : 'bool' | 'byte' | 'int' | 'uint' 

    | 'long' | 'ulong' | 'char' | 'string' | 'void'

    ;



arrayDims

    : ('[' (',' )* ']')+ -> ^(ARRAY_TYPE ('[' (',' )* ']')+)

    ;



// === Variable declarations ===

varDecl

    : typeRef varInit (',' varInit)* ';' -> ^(VAR_DECL typeRef varInit+)

    ;



varInit

    : ID arrayDecl? ('=' expr)? -> ^(VAR_INIT ID arrayDecl? expr?)

    ;

arrayDecl
    : '[' expr? ']' -> ^(ARRAY_DECL expr?)
    ;



// === Statements ===

statement

    : ifStatement

    | ( typeRef ID )=> varDecl

    | block

    | whileStatement

    | doWhileStatement

    | breakStatement

    | continueStatement      // здесь

    | returnStatement        // и здесь

    | exprStatement

    ;



statementNoShortIf

    : ( typeRef ID )=> varDecl

    | block

    | whileStatementNoShortIf

    | doWhileStatement

    | breakStatement

    | continueStatement      // и тут

    | returnStatement        // и тут

    | exprStatement

    | ifStatementNoShortIf

    ;



// === return / continue ===

returnStatement

    : 'return' expr? ';'

      -> ^(RETURN expr?)

    ;



continueStatement

    : 'continue' ';'

      -> ^(CONTINUE)

    ;





// --- Dangling else resolution ---

ifStatement

    : 'if' '(' expr ')' thenStmt=statementNoShortIf

      ( 'else' elseStmt=statement

        -> ^(IF expr $thenStmt $elseStmt)

      |

        -> ^(IF expr $thenStmt)

      )

    ;



ifStatementNoShortIf

    : 'if' '(' expr ')' thenStmt=statementNoShortIf 'else' elseStmt=statementNoShortIf

      -> ^(IF expr $thenStmt $elseStmt)

    ;





// === Loops ===

whileStatement

    : 'while' '(' expr ')' statement

      -> ^(WHILE expr statement)

    ;



whileStatementNoShortIf

    : 'while' '(' expr ')' statementNoShortIf

      -> ^(WHILE expr statementNoShortIf)

    ;



doWhileStatement

    : 'do' block 'while' '(' expr ')' ';'

      -> ^(DO_WHILE block expr)

    ;



breakStatement    : 'break' ';'    -> ^(BREAK) ;

exprStatement     : expr ';'       -> ^(EXPR_STMT expr) ;



// === Blocks ===

block : '{' statement* '}' -> ^(BLOCK statement*) ;



// === Expressions ===

expr : assignExpr ;



assignExpr       : logicalOrExpr ('='^ assignExpr)? ;

logicalOrExpr    : logicalAndExpr ('||'^ logicalAndExpr)* ;

logicalAndExpr   : bitwiseOrExpr ('&&'^ bitwiseOrExpr)* ;

bitwiseOrExpr    : bitwiseXorExpr ('|' ^ bitwiseXorExpr)* ;

bitwiseXorExpr   : bitwiseAndExpr ('^' ^ bitwiseAndExpr)* ;

bitwiseAndExpr   : equalityExpr ('&' ^ equalityExpr)* ;

equalityExpr     : relationalExpr (('=='^ | '!='^) relationalExpr)* ;

relationalExpr   : shiftExpr (('<'^ | '>'^ | '<='^ | '>='^) shiftExpr)* ;

shiftExpr        : additiveExpr (('<<'^ | '>>'^) additiveExpr)* ;

additiveExpr     : multiplicativeExpr (('+'^ | '-'^) multiplicativeExpr)* ;

multiplicativeExpr

    : unaryExpr (('*'^ | '/'^ | '%'^) unaryExpr)*

    ;

unaryExpr

    : AWAIT_KW unaryExpr     -> ^(AWAIT unaryExpr)

    | unaryOp unaryExpr      -> ^(UNARY unaryOp unaryExpr)

    | postfixExpr

    ;

unaryOp : '+' | '-' | '!' | '~' | '++' | '--' ;



postfixExpr : primaryExpr postfixOp* ;
postfixOp
    : '(' exprList ')' -> ^(CALL exprList)
    | '[' exprList ']' -> ^(ARRAY_INDEX exprList)
    | '.' ID           -> ^(MEMBER_ACCESS ID)
    | '++'
    | '--'
    ;


exprList : (expr (',' expr)*)? -> ^(EXPR_LIST expr*) ;



primaryExpr : literal | ID | '(' expr ')' ;



literal : DEC | HEX | BITS | CHAR | STRING | BOOL ;

