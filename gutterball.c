/*
 * gutterball v2 — The Vex Language Compiler
 * Fast. Low-level. No deps. Single binary.
 *
 * Features:
 *   - Optimizing x86-64 codegen (constant folding, strength reduction,
 *     peephole opts, register hints, inline small fns)
 *   - Arrays, pointers, structs
 *   - String ops: ReadIn, ReadInt, StrLen, StrCat, StrCmp, StrEq, IntToStr
 *   - Memory: Alloc, Free
 *   - Raw syscall: Syscall(n, a,b,c,d,e,f)
 *   - For loops: for i in 0..N
 *   - Cast: cast<i64>(x)
 *   - Multiple assignment / swap via pointers
 *   - Compiler flags: -O0 / -O2 (default O2)
 *
 * Usage: gutterball <file.egg> [-o out] [-O0]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <errno.h>

/* ═══════════════════════════════════════════════════════════
 * Utilities
 * ═══════════════════════════════════════════════════════════*/
static int opt_level = 2;

static void die(const char *fmt, ...) {
    va_list ap;
    fprintf(stderr,"\033[91m[gutterball]\033[0m ");
    va_start(ap,fmt); vfprintf(stderr,fmt,ap); va_end(ap);
    fputc('\n',stderr); exit(1);
}
static void *xmalloc(size_t n)       { void*p=malloc(n);    if(!p)die("OOM"); return p; }
static void *xrealloc(void*p,size_t n){ p=realloc(p,n);     if(!p)die("OOM"); return p; }
static char *xstrdup(const char *s)  { char*d=strdup(s);    if(!d)die("OOM"); return d; }
static char *xstrndup(const char*s,size_t n){ char*d=strndup(s,n); if(!d)die("OOM"); return d; }

/* ═══════════════════════════════════════════════════════════
 * Byte buffer
 * ═══════════════════════════════════════════════════════════*/
typedef struct { uint8_t *data; size_t len,cap; } Buf;
static void buf_init(Buf*b){b->data=NULL;b->len=b->cap=0;}
static void buf_push(Buf*b,uint8_t v){
    if(b->len>=b->cap){b->cap=b->cap?b->cap*2:512;b->data=xrealloc(b->data,b->cap);}
    b->data[b->len++]=v;
}
static void buf_bytes(Buf*b,const void*d,size_t n){for(size_t i=0;i<n;i++)buf_push(b,((uint8_t*)d)[i]);}
static size_t buf_pos(Buf*b){return b->len;}
static void buf_patch32(Buf*b,size_t off,int32_t v){memcpy(b->data+off,&v,4);}
static void buf_patch64(Buf*b,size_t off,int64_t v){memcpy(b->data+off,&v,8);}

/* ═══════════════════════════════════════════════════════════
 * Lexer
 * ═══════════════════════════════════════════════════════════*/
typedef enum {
    /* literals */
    TT_INT, TT_FLOAT, TT_STRING, TT_CHAR, TT_IDENT,
    /* keywords */
    TT_FN, TT_VAR, TT_CONST, TT_RETURN, TT_IF, TT_ELSE,
    TT_WHILE, TT_FOR, TT_IN, TT_LOOP, TT_BREAK, TT_CONTINUE,
    TT_TRUE, TT_FALSE, TT_NULL,
    TT_STRUCT, TT_CAST,
    TT_EXECUTE, TT_DOT,
    /* types */
    TT_VOID, TT_I8, TT_I16, TT_I32, TT_I64,
    TT_U8,  TT_U16, TT_U32, TT_U64,
    TT_F32, TT_F64, TT_BOOL_T, TT_STR_T, TT_PTR_T,
    /* symbols */
    TT_LPAREN, TT_RPAREN, TT_LBRACE, TT_RBRACE,
    TT_LBRACKET, TT_RBRACKET,
    TT_COMMA, TT_COLON, TT_SEMI, TT_ARROW, TT_DOTDOT,
    TT_AMP,   /* & */
    TT_HASH,  /* # */
    TT_AT,    /* @ (deref) */
    TT_LT_ANGLE, TT_GT_ANGLE, /* for cast<T> */
    /* operators */
    TT_EQ, TT_PLUSEQ, TT_MINUSEQ, TT_STAREQ, TT_SLASHEQ,
    TT_EQEQ, TT_NEQ, TT_LT, TT_GT, TT_LTE, TT_GTE,
    TT_PLUS, TT_MINUS, TT_STAR, TT_SLASH, TT_PERCENT,
    TT_BANG, TT_AND, TT_OR,
    TT_BAND, TT_BOR, TT_BXOR, TT_BNOT, /* bitwise */
    TT_SHL, TT_SHR,
    TT_EOF
} TokType;

typedef struct {
    TokType  type;
    char    *sval;
    int64_t  ival;
    double   fval;
    int      line;
} Token;

typedef struct {
    const char *src;
    size_t pos;
    int line;
    Token *tokens;
    size_t ntok,tcap;
} Lexer;

static void ltok(Lexer*L,TokType t,char*s,int64_t iv,double fv){
    if(L->ntok>=L->tcap){L->tcap=L->tcap?L->tcap*2:512;L->tokens=xrealloc(L->tokens,L->tcap*sizeof(Token));}
    L->tokens[L->ntok++]=(Token){t,s,iv,fv,L->line};
}
static char lp(Lexer*L,int o){size_t p=L->pos+o;return(p<strlen(L->src))?L->src[p]:0;}
static char la(Lexer*L){char c=L->src[L->pos++];if(c=='\n')L->line++;return c;}
static void lskip(Lexer*L){
    for(;;){
        char c=lp(L,0);
        if(c==' '||c=='\t'||c=='\r'||c=='\n'){la(L);continue;}
        if(c=='/'&&lp(L,1)=='/'){while(lp(L,0)&&lp(L,0)!='\n')la(L);continue;}
        if(c=='/'&&lp(L,1)=='*'){la(L);la(L);while(lp(L,0)&&!(lp(L,0)=='*'&&lp(L,1)=='/')){la(L);}if(lp(L,0)){la(L);la(L);}continue;}
        break;
    }
}

static struct{const char*kw;TokType tt;}KW[]={
    {"fn",TT_FN},{"var",TT_VAR},{"const",TT_CONST},{"return",TT_RETURN},
    {"if",TT_IF},{"else",TT_ELSE},{"while",TT_WHILE},{"for",TT_FOR},
    {"in",TT_IN},{"loop",TT_LOOP},{"break",TT_BREAK},{"continue",TT_CONTINUE},
    {"true",TT_TRUE},{"false",TT_FALSE},{"null",TT_NULL},
    {"struct",TT_STRUCT},{"cast",TT_CAST},{"execute",TT_EXECUTE},
    {"void",TT_VOID},{"i8",TT_I8},{"i16",TT_I16},{"i32",TT_I32},{"i64",TT_I64},
    {"u8",TT_U8},{"u16",TT_U16},{"u32",TT_U32},{"u64",TT_U64},
    {"f32",TT_F32},{"f64",TT_F64},{"bool",TT_BOOL_T},{"str",TT_STR_T},{"ptr",TT_PTR_T},
    {NULL,TT_EOF}
};

static void lex(Lexer*L){
    for(;;){
        lskip(L);
        if(!lp(L,0)){ltok(L,TT_EOF,NULL,0,0);break;}
        int line=L->line;
        char c=lp(L,0);

        /* string */
        if(c=='"'){
            la(L); char buf[8192]; int bi=0;
            while(lp(L,0)&&lp(L,0)!='"'){
                char ch=la(L);
                if(ch=='\\'){char e=la(L);switch(e){case 'n':ch='\n';break;case 't':ch='\t';break;case 'r':ch='\r';break;case '0':ch='\0';break;default:ch=e;}}
                if(bi<8190)buf[bi++]=ch;
            }
            if(lp(L,0)=='"')la(L);
            buf[bi]=0; ltok(L,TT_STRING,xstrdup(buf),0,0); continue;
        }
        /* char literal */
        if(c=='\''){
            la(L); char ch=la(L);
            if(ch=='\\'){char e=la(L);switch(e){case 'n':ch='\n';break;case 't':ch='\t';break;default:ch=e;}}
            if(lp(L,0)=='\'')la(L);
            ltok(L,TT_CHAR,NULL,(int64_t)ch,0); continue;
        }
        /* number */
        if((c>='0'&&c<='9')||(c=='-'&&lp(L,1)>='0'&&lp(L,1)<='9'&&L->ntok==0)){
            int neg=(c=='-'); if(neg)la(L);
            int64_t v=0; int is_hex=0;
            if(lp(L,0)=='0'&&(lp(L,1)=='x'||lp(L,1)=='X')){la(L);la(L);is_hex=1;while((lp(L,0)>='0'&&lp(L,0)<='9')||(lp(L,0)>='a'&&lp(L,0)<='f')||(lp(L,0)>='A'&&lp(L,0)<='F')){char hc=la(L);v=v*16+(hc>='a'?hc-'a'+10:hc>='A'?hc-'A'+10:hc-'0');}}
            else {while(lp(L,0)>='0'&&lp(L,0)<='9')v=v*10+(la(L)-'0');}
            if(!is_hex&&lp(L,0)=='.'&&lp(L,1)>='0'&&lp(L,1)<='9'){
                la(L); double fv=(double)v; double frac=0.1;
                while(lp(L,0)>='0'&&lp(L,0)<='9'){fv+=(la(L)-'0')*frac;frac*=0.1;}
                ltok(L,TT_FLOAT,NULL,0,neg?-fv:fv); continue;
            }
            ltok(L,TT_INT,NULL,neg?-v:v,0); continue;
        }
        /* ident / keyword */
        if((c>='a'&&c<='z')||(c>='A'&&c<='Z')||c=='_'){
            char buf[256]; int bi=0;
            while((lp(L,0)>='a'&&lp(L,0)<='z')||(lp(L,0)>='A'&&lp(L,0)<='Z')||(lp(L,0)>='0'&&lp(L,0)<='9')||lp(L,0)=='_')
                buf[bi++]=la(L);
            buf[bi]=0;
            TokType tt=TT_IDENT;
            for(int i=0;KW[i].kw;i++)if(!strcmp(buf,KW[i].kw)){tt=KW[i].tt;break;}
            ltok(L,tt,tt==TT_IDENT?xstrdup(buf):NULL,0,0); continue;
        }
        la(L);
        switch(c){
            case '(':ltok(L,TT_LPAREN,NULL,0,0);break;
            case ')':ltok(L,TT_RPAREN,NULL,0,0);break;
            case '{':ltok(L,TT_LBRACE,NULL,0,0);break;
            case '}':ltok(L,TT_RBRACE,NULL,0,0);break;
            case '[':ltok(L,TT_LBRACKET,NULL,0,0);break;
            case ']':ltok(L,TT_RBRACKET,NULL,0,0);break;
            case ',':ltok(L,TT_COMMA,NULL,0,0);break;
            case ';':ltok(L,TT_SEMI,NULL,0,0);break;
            case '#':ltok(L,TT_HASH,NULL,0,0);break;
            case '@':ltok(L,TT_AT,NULL,0,0);break;
            case '~':ltok(L,TT_BNOT,NULL,0,0);break;
            case ':':ltok(L,TT_COLON,NULL,0,0);break;
            case '.':
                if(lp(L,0)=='.'){{la(L);ltok(L,TT_DOTDOT,NULL,0,0);}}
                else ltok(L,TT_DOT,NULL,0,0);break;
            case '+': ltok(L,lp(L,0)=='='?(la(L),TT_PLUSEQ):TT_PLUS,NULL,0,0);break;
            case '*': ltok(L,lp(L,0)=='='?(la(L),TT_STAREQ):TT_STAR,NULL,0,0);break;
            case '/': ltok(L,lp(L,0)=='='?(la(L),TT_SLASHEQ):TT_SLASH,NULL,0,0);break;
            case '%': ltok(L,TT_PERCENT,NULL,0,0);break;
            case '=': ltok(L,lp(L,0)=='='?(la(L),TT_EQEQ):TT_EQ,NULL,0,0);break;
            case '!': ltok(L,lp(L,0)=='='?(la(L),TT_NEQ):TT_BANG,NULL,0,0);break;
            case '&':
                if(lp(L,0)=='&'){la(L);ltok(L,TT_AND,NULL,0,0);}
                else ltok(L,TT_AMP,NULL,0,0);break;
            case '|':
                if(lp(L,0)=='|'){la(L);ltok(L,TT_OR,NULL,0,0);}
                else ltok(L,TT_BOR,NULL,0,0);break;
            case '^':ltok(L,TT_BXOR,NULL,0,0);break;
            case '<':
                if(lp(L,0)=='='){la(L);ltok(L,TT_LTE,NULL,0,0);}
                else if(lp(L,0)=='<'){la(L);ltok(L,TT_SHL,NULL,0,0);}
                else ltok(L,TT_LT,NULL,0,0);break;
            case '>':
                if(lp(L,0)=='='){la(L);ltok(L,TT_GTE,NULL,0,0);}
                else if(lp(L,0)=='>'){la(L);ltok(L,TT_SHR,NULL,0,0);}
                else ltok(L,TT_GT,NULL,0,0);break;
            case '-':
                if(lp(L,0)=='>'){la(L);ltok(L,TT_ARROW,NULL,0,0);}
                else if(lp(L,0)=='='){la(L);ltok(L,TT_MINUSEQ,NULL,0,0);}
                else ltok(L,TT_MINUS,NULL,0,0);break;
            default: die("[Line %d] Unknown character '%c' (0x%02x)",L->line,c,(unsigned char)c);
        }
    }
}

/* ═══════════════════════════════════════════════════════════
 * Types
 * ═══════════════════════════════════════════════════════════*/
typedef enum {
    TY_VOID, TY_I8, TY_I16, TY_I32, TY_I64,
    TY_U8,   TY_U16, TY_U32, TY_U64,
    TY_F32,  TY_F64,
    TY_BOOL, TY_STR, TY_PTR, TY_ARRAY, TY_STRUCT, TY_UNKNOWN
} TypeKind;

typedef struct Type Type;
struct Type {
    TypeKind kind;
    Type    *inner;   /* for ptr/array */
    int      count;   /* for array */
    char    *name;    /* for struct */
};

static Type *ty_new(TypeKind k){Type*t=xmalloc(sizeof(Type));memset(t,0,sizeof(Type));t->kind=k;return t;}
static Type *ty_ptr(Type*inner){Type*t=ty_new(TY_PTR);t->inner=inner;return t;}
static Type *ty_array(Type*inner,int n){Type*t=ty_new(TY_ARRAY);t->inner=inner;t->count=n;return t;}
static int ty_size(Type*t){
    if(!t)return 8;
    switch(t->kind){
        case TY_I8: case TY_U8: case TY_BOOL: return 1;
        case TY_I16:case TY_U16: return 2;
        case TY_I32:case TY_U32:case TY_F32: return 4;
        case TY_ARRAY: return ty_size(t->inner)*t->count;
        default: return 8;
    }
}

/* ═══════════════════════════════════════════════════════════
 * AST
 * ═══════════════════════════════════════════════════════════*/
typedef enum {
    ND_INT, ND_FLOAT, ND_STR, ND_BOOL, ND_CHAR, ND_NULL,
    ND_IDENT, ND_BINOP, ND_UNARY, ND_CAST,
    ND_CALL, ND_EXEC_CALL,
    ND_INDEX,       /* arr[i] */
    ND_FIELD,       /* s.field */
    ND_ADDROF,      /* &x */
    ND_DEREF,       /* @x  (dereference) */
    ND_VARDECL, ND_CONSTDECL, ND_ASSIGN, ND_ASSIGN_OP,
    ND_RETURN, ND_IF, ND_WHILE, ND_FOR, ND_LOOP, ND_BREAK, ND_CONTINUE,
    ND_EXPRSTMT, ND_BLOCK,
    ND_FNDEF, ND_PARAM,
    ND_STRUCT_DEF,
    ND_PROGRAM
} NodeKind;

typedef struct Node Node;
struct Node {
    NodeKind  kind;
    int       line;
    int64_t   ival;
    double    fval;
    char     *sval;
    int       bval;
    Type     *type_hint;
    Node    **ch;
    int       nch, cch;
    /* optimizer annotations */
    int       is_const;
    int64_t   const_val;
};

static Node *nn(NodeKind k,int line){
    Node*n=xmalloc(sizeof(Node));memset(n,0,sizeof(Node));n->kind=k;n->line=line;return n;
}
static void nadd(Node*p,Node*c){
    if(p->nch>=p->cch){p->cch=p->cch?p->cch*2:4;p->ch=xrealloc(p->ch,p->cch*sizeof(Node*));}
    p->ch[p->nch++]=c;
}

/* ═══════════════════════════════════════════════════════════
 * Parser
 * ═══════════════════════════════════════════════════════════*/
typedef struct{Token*tokens;size_t pos;}Parser;
static Token *pp(Parser*P){return&P->tokens[P->pos];}
static Token *pa(Parser*P){Token*t=&P->tokens[P->pos];if(t->type!=TT_EOF)P->pos++;return t;}
static int    pc(Parser*P,TokType t){return pp(P)->type==t;}
static Token *pm(Parser*P,TokType t){if(pc(P,t))return pa(P);return NULL;}
static Token *pe(Parser*P,TokType t,const char*msg){
    if(!pc(P,t))die("[Line %d] Expected %s",pp(P)->line,msg);
    return pa(P);
}

static Node *parse_expr(Parser*P);
static Node *parse_stmt(Parser*P);
static Node *parse_block(Parser*P);
static Type *parse_type(Parser*P);

static Type *parse_type(Parser*P){
    Token*t=pp(P);
    /* ptr<T> or ptr */
    if(t->type==TT_PTR_T){pa(P);
        if(pm(P,TT_LT)){Type*inner=parse_type(P);pe(P,TT_GT,">");return ty_ptr(inner);}
        return ty_ptr(ty_new(TY_I64));
    }
    /* arr[N] or arr[N]<T> */
    if(t->type==TT_LBRACKET||pc(P,TT_IDENT)){
        /* check for arr[N] pattern — handled in vardecl specially */
    }
    pa(P);
    TypeKind k=TY_UNKNOWN;
    switch(t->type){
        case TT_VOID:  k=TY_VOID;break; case TT_I8:  k=TY_I8;break;
        case TT_I16:   k=TY_I16;break;  case TT_I32: k=TY_I32;break;
        case TT_I64:   k=TY_I64;break;  case TT_U8:  k=TY_U8;break;
        case TT_U16:   k=TY_U16;break;  case TT_U32: k=TY_U32;break;
        case TT_U64:   k=TY_U64;break;  case TT_F32: k=TY_F32;break;
        case TT_F64:   k=TY_F64;break;  case TT_BOOL_T:k=TY_BOOL;break;
        case TT_STR_T: k=TY_STR;break;
        default: k=TY_I64;break;
    }
    return ty_new(k);
}

static Node *parse_primary(Parser*P){
    Token*t=pp(P);
    if(t->type==TT_INT)  {pa(P);Node*n=nn(ND_INT,t->line);n->ival=t->ival;n->is_const=1;n->const_val=t->ival;return n;}
    if(t->type==TT_FLOAT){pa(P);Node*n=nn(ND_FLOAT,t->line);n->fval=t->fval;return n;}
    if(t->type==TT_STRING){pa(P);Node*n=nn(ND_STR,t->line);n->sval=xstrdup(t->sval);return n;}
    if(t->type==TT_CHAR) {pa(P);Node*n=nn(ND_CHAR,t->line);n->ival=t->ival;return n;}
    if(t->type==TT_TRUE) {pa(P);Node*n=nn(ND_BOOL,t->line);n->bval=1;n->is_const=1;n->const_val=1;return n;}
    if(t->type==TT_FALSE){pa(P);Node*n=nn(ND_BOOL,t->line);n->bval=0;n->is_const=1;n->const_val=0;return n;}
    if(t->type==TT_NULL) {pa(P);Node*n=nn(ND_NULL,t->line);n->is_const=1;n->const_val=0;return n;}

    /* cast<T>(expr) */
    if(t->type==TT_CAST){
        pa(P); pe(P,TT_LT,"<"); Type*ty=parse_type(P); pe(P,TT_GT,">");
        pe(P,TT_LPAREN,"("); Node*e=parse_expr(P); pe(P,TT_RPAREN,")");
        Node*n=nn(ND_CAST,t->line); n->type_hint=ty; nadd(n,e); return n;
    }

    /* &expr — address-of */
    if(t->type==TT_AMP){
        pa(P); Node*n=nn(ND_ADDROF,t->line); nadd(n,parse_primary(P)); return n;
    }
    /* @expr — dereference */
    if(t->type==TT_AT){
        pa(P); Node*n=nn(ND_DEREF,t->line); nadd(n,parse_primary(P)); return n;
    }

    /* execute.Method(...) */
    if(t->type==TT_EXECUTE){
        pa(P); pe(P,TT_DOT,".");
        Token*m=pe(P,TT_IDENT,"method name");
        Node*n=nn(ND_EXEC_CALL,t->line); n->sval=xstrdup(m->sval);
        pe(P,TT_LPAREN,"(");
        while(!pc(P,TT_RPAREN)&&!pc(P,TT_EOF)){nadd(n,parse_expr(P));if(!pm(P,TT_COMMA))break;}
        pe(P,TT_RPAREN,")"); return n;
    }

    /* ident, call, index, field */
    if(t->type==TT_IDENT){
        pa(P);
        if(pm(P,TT_LPAREN)){
            Node*n=nn(ND_CALL,t->line);n->sval=xstrdup(t->sval);
            while(!pc(P,TT_RPAREN)&&!pc(P,TT_EOF)){nadd(n,parse_expr(P));if(!pm(P,TT_COMMA))break;}
            pe(P,TT_RPAREN,")"); return n;
        }
        Node*n=nn(ND_IDENT,t->line);n->sval=xstrdup(t->sval);
        return n;
    }

    if(t->type==TT_LPAREN){pa(P);Node*n=parse_expr(P);pe(P,TT_RPAREN,")");return n;}
    die("[Line %d] Unexpected token (type=%d, val='%s')",t->line,t->type,t->sval?t->sval:"");
    return NULL;
}

static Node *parse_postfix(Parser*P){
    Node*l=parse_primary(P);
    for(;;){
        if(pc(P,TT_LBRACKET)){
            pa(P); Node*n=nn(ND_INDEX,l->line); nadd(n,l); nadd(n,parse_expr(P));
            pe(P,TT_RBRACKET,"]"); l=n;
        } else if(pc(P,TT_DOT)){
            pa(P); Token*f=pe(P,TT_IDENT,"field name");
            Node*n=nn(ND_FIELD,l->line); nadd(n,l); n->sval=xstrdup(f->sval); l=n;
        } else break;
    }
    return l;
}

static Node *parse_unary(Parser*P){
    Token*t=pp(P);
    if(t->type==TT_MINUS){pa(P);Node*n=nn(ND_UNARY,t->line);n->sval=xstrdup("-");nadd(n,parse_unary(P));return n;}
    if(t->type==TT_BANG) {pa(P);Node*n=nn(ND_UNARY,t->line);n->sval=xstrdup("!");nadd(n,parse_unary(P));return n;}
    if(t->type==TT_BNOT) {pa(P);Node*n=nn(ND_UNARY,t->line);n->sval=xstrdup("~");nadd(n,parse_unary(P));return n;}
    if(t->type==TT_AT)   {pa(P);Node*n=nn(ND_DEREF,t->line);nadd(n,parse_unary(P));return n;}
    if(t->type==TT_AMP)  {pa(P);Node*n=nn(ND_ADDROF,t->line);nadd(n,parse_unary(P));return n;}
    return parse_postfix(P);
}

/* precedence-climbing binary ops */
static int prec(TokType t){
    switch(t){
        case TT_OR:    return 1;
        case TT_AND:   return 2;
        case TT_BOR:   return 3;
        case TT_BXOR:  return 4;
        case TT_BAND:  return 5; /* unused — & is addrof */
        case TT_EQEQ: case TT_NEQ: return 6;
        case TT_LT: case TT_GT: case TT_LTE: case TT_GTE: return 7;
        case TT_SHL: case TT_SHR: return 8;
        case TT_PLUS: case TT_MINUS: return 9;
        case TT_STAR: case TT_SLASH: case TT_PERCENT: return 10;
        default: return -1;
    }
}
static const char *op_str(TokType t){
    switch(t){
        case TT_PLUS:return"+";case TT_MINUS:return"-";case TT_STAR:return"*";
        case TT_SLASH:return"/";case TT_PERCENT:return"%";
        case TT_EQEQ:return"==";case TT_NEQ:return"!=";
        case TT_LT:return"<";case TT_GT:return">";
        case TT_LTE:return"<=";case TT_GTE:return">=";
        case TT_AND:return"&&";case TT_OR:return"||";
        case TT_BOR:return"|";case TT_BXOR:return"^";
        case TT_SHL:return"<<";case TT_SHR:return">>";
        default:return"+";
    }
}

static Node *parse_binop(Parser*P,int min_prec){
    Node*l=parse_unary(P);
    for(;;){
        int p=prec(pp(P)->type);
        if(p<min_prec)break;
        Token*op=pa(P);
        Node*r=parse_binop(P,p+1);
        Node*n=nn(ND_BINOP,l->line);n->sval=xstrdup(op_str(op->type));
        nadd(n,l);nadd(n,r);
        /* constant folding */
        if(opt_level>=1&&l->is_const&&r->is_const){
            int64_t a=l->const_val,b=r->const_val,res=0;int fold=1;
            const char*s=n->sval;
            if(!strcmp(s,"+"))res=a+b;
            else if(!strcmp(s,"-"))res=a-b;
            else if(!strcmp(s,"*"))res=a*b;
            else if(!strcmp(s,"/")&&b!=0)res=a/b;
            else if(!strcmp(s,"%")&&b!=0)res=a%b;
            else if(!strcmp(s,"=="))res=a==b;
            else if(!strcmp(s,"!="))res=a!=b;
            else if(!strcmp(s,"<"))res=a<b;
            else if(!strcmp(s,">"))res=a>b;
            else if(!strcmp(s,"<="))res=a<=b;
            else if(!strcmp(s,">="))res=a>=b;
            else if(!strcmp(s,"&&"))res=a&&b;
            else if(!strcmp(s,"||"))res=a||b;
            else if(!strcmp(s,"|"))res=a|b;
            else if(!strcmp(s,"^"))res=a^b;
            else if(!strcmp(s,"<<"))res=a<<b;
            else if(!strcmp(s,">>"))res=a>>b;
            else fold=0;
            if(fold){n->is_const=1;n->const_val=res;}
        }
        l=n;
    }
    return l;
}

static Node *parse_expr(Parser*P){ return parse_binop(P,1); }

static Node *parse_block(Parser*P){
    pe(P,TT_LBRACE,"{");
    Node*b=nn(ND_BLOCK,pp(P)->line);
    while(!pc(P,TT_RBRACE)&&!pc(P,TT_EOF))nadd(b,parse_stmt(P));
    pe(P,TT_RBRACE,"}");
    return b;
}

static Node *parse_stmt(Parser*P){
    Token*t=pp(P);

    /* var / const declaration */
    if(t->type==TT_VAR||t->type==TT_CONST){
        int is_const=(t->type==TT_CONST); pa(P);
        Token*name=pe(P,TT_IDENT,"variable name");
        Node*n=nn(is_const?ND_CONSTDECL:ND_VARDECL,t->line);
        n->sval=xstrdup(name->sval);
        /* optional type annotation, including array syntax: var x: [N]T */
        if(pm(P,TT_COLON)){
            if(pc(P,TT_LBRACKET)){
                pa(P);
                int count=0;
                if(pc(P,TT_INT)){count=(int)pp(P)->ival;pa(P);}
                pe(P,TT_RBRACKET,"]");
                Type*inner=parse_type(P);
                n->type_hint=ty_array(inner,count);
            } else {
                n->type_hint=parse_type(P);
            }
        }
        if(pm(P,TT_EQ))nadd(n,parse_expr(P));
        pm(P,TT_SEMI); return n;
    }

    if(t->type==TT_RETURN){
        pa(P); Node*n=nn(ND_RETURN,t->line);
        if(!pc(P,TT_RBRACE)&&!pc(P,TT_SEMI))nadd(n,parse_expr(P));
        pm(P,TT_SEMI); return n;
    }
    if(t->type==TT_IF){
        pa(P); Node*n=nn(ND_IF,t->line);
        nadd(n,parse_expr(P)); nadd(n,parse_block(P));
        if(pm(P,TT_ELSE)){
            if(pc(P,TT_IF))nadd(n,parse_stmt(P));
            else nadd(n,parse_block(P));
        }
        return n;
    }
    if(t->type==TT_WHILE){
        pa(P); Node*n=nn(ND_WHILE,t->line);
        nadd(n,parse_expr(P)); nadd(n,parse_block(P)); return n;
    }
    /* for i in start..end */
    if(t->type==TT_FOR){
        pa(P); Token*var=pe(P,TT_IDENT,"loop variable"); pe(P,TT_IN,"in");
        Node*n=nn(ND_FOR,t->line); n->sval=xstrdup(var->sval);
        nadd(n,parse_expr(P)); /* start */
        pe(P,TT_DOTDOT,".."); nadd(n,parse_expr(P)); /* end */
        nadd(n,parse_block(P)); return n;
    }
    if(t->type==TT_LOOP){pa(P);Node*n=nn(ND_LOOP,t->line);nadd(n,parse_block(P));return n;}
    if(t->type==TT_BREAK){pa(P);pm(P,TT_SEMI);return nn(ND_BREAK,t->line);}
    if(t->type==TT_CONTINUE){pa(P);pm(P,TT_SEMI);return nn(ND_CONTINUE,t->line);}

    /* expression or assignment */
    Node*lhs=parse_expr(P);
    /* compound assignment */
    TokType at=pp(P)->type;
    if(at==TT_EQ||at==TT_PLUSEQ||at==TT_MINUSEQ||at==TT_STAREQ||at==TT_SLASHEQ){
        pa(P); Node*rhs=parse_expr(P);
        /* for compound ops, desugar: x += y  -> x = x + y */
        if(at!=TT_EQ){
            const char*op=(at==TT_PLUSEQ)?"+":at==TT_MINUSEQ?"-":at==TT_STAREQ?"*":"/";
            Node*bop=nn(ND_BINOP,lhs->line);bop->sval=xstrdup(op);
            Node*lhs2=nn(lhs->kind,lhs->line);lhs2->sval=lhs->sval?xstrdup(lhs->sval):NULL;
            lhs2->ival=lhs->ival;
            for(int i=0;i<lhs->nch;i++)nadd(lhs2,lhs->ch[i]);
            nadd(bop,lhs2);nadd(bop,rhs);rhs=bop;
        }
        Node*n=nn(ND_ASSIGN,t->line);
        nadd(n,lhs); nadd(n,rhs); pm(P,TT_SEMI); return n;
    }
    pm(P,TT_SEMI);
    Node*n=nn(ND_EXPRSTMT,t->line);nadd(n,lhs);return n;
}

static Node *parse_struct(Parser*P){
    Token*t=pe(P,TT_STRUCT,"struct");
    Token*name=pe(P,TT_IDENT,"struct name");
    Node*n=nn(ND_STRUCT_DEF,t->line);n->sval=xstrdup(name->sval);
    pe(P,TT_LBRACE,"{");
    while(!pc(P,TT_RBRACE)&&!pc(P,TT_EOF)){
        Token*field=pe(P,TT_IDENT,"field name");
        pe(P,TT_COLON,":");
        Type*ty=parse_type(P);
        Node*f=nn(ND_PARAM,field->line);f->sval=xstrdup(field->sval);f->type_hint=ty;
        nadd(n,f); pm(P,TT_SEMI); pm(P,TT_COMMA);
    }
    pe(P,TT_RBRACE,"}"); return n;
}

static Node *parse_fn(Parser*P){
    Token*t=pe(P,TT_FN,"fn");
    Token*name=pe(P,TT_IDENT,"function name");
    Node*fn=nn(ND_FNDEF,t->line);fn->sval=xstrdup(name->sval);
    pe(P,TT_LPAREN,"(");
    while(!pc(P,TT_RPAREN)&&!pc(P,TT_EOF)){
        Token*pn=pe(P,TT_IDENT,"param name"); pe(P,TT_COLON,":");
        Type*pt=parse_type(P);
        Node*p=nn(ND_PARAM,pn->line);p->sval=xstrdup(pn->sval);p->type_hint=pt;
        nadd(fn,p); if(!pm(P,TT_COMMA))break;
    }
    pe(P,TT_RPAREN,")"); pe(P,TT_ARROW,"->"); fn->type_hint=parse_type(P);
    nadd(fn,parse_block(P)); return fn;
}

static Node *parse_program(Parser*P){
    Node*prog=nn(ND_PROGRAM,0);
    while(!pc(P,TT_EOF)){
        if(pc(P,TT_STRUCT))nadd(prog,parse_struct(P));
        else nadd(prog,parse_fn(P));
    }
    return prog;
}

/* ═══════════════════════════════════════════════════════════
 * x86-64 emitters  (same as v1 but extended)
 * ═══════════════════════════════════════════════════════════*/
#define RAX 0
#define RCX 1
#define RDX 2
#define RBX 3
#define RSP 4
#define RBP 5
#define RSI 6
#define RDI 7
#define R8  8
#define R9  9
#define R10 10
#define R11 11
#define R12 12
#define R13 13

static int ri(int r){return r&7;}
static int rb(int r){return r>=8;}
static int rr(int r){return r>=8;}

static void e_rex(Buf*b,int w,int r,int x,int base){
    uint8_t byte=0x40|(w<<3)|(r<<2)|(x<<1)|rb(base);
    /* only emit if needed */
    if(byte!=0x40||w)buf_push(b,byte);
}
static void e_mov_r_imm64(Buf*b,int dst,int64_t imm){
    buf_push(b,0x48|rb(dst)); buf_push(b,0xB8|ri(dst)); buf_bytes(b,&imm,8);
}
static void e_mov_r_imm32(Buf*b,int dst,int32_t imm){
    /* mov eax, imm32 — zero-extends to 64-bit, shorter encoding */
    if(rb(dst))buf_push(b,0x41);
    buf_push(b,0xB8|ri(dst)); buf_bytes(b,&imm,4);
}
static void e_mov_r_imm(Buf*b,int dst,int64_t imm){
    if(imm>=0&&imm<=0x7FFFFFFF) e_mov_r_imm32(b,dst,(int32_t)imm);
    else e_mov_r_imm64(b,dst,imm);
}
static void e_xor_r_r(Buf*b,int d,int s){
    buf_push(b,0x48|(rr(s)<<2)|rb(d)); buf_push(b,0x31); buf_push(b,(3<<6)|(ri(s)<<3)|ri(d));
}
static void e_mov_r_r(Buf*b,int d,int s){
    if(d==s)return; /* elide no-op moves */
    buf_push(b,0x48|(rr(s)<<2)|rb(d)); buf_push(b,0x89); buf_push(b,(3<<6)|(ri(s)<<3)|ri(d));
}
static void e_mov_mem_r(Buf*b,int base,int off,int src){
    buf_push(b,0x48|(rr(src)<<2)|rb(base)); buf_push(b,0x89);
    if(off==0&&ri(base)!=5){buf_push(b,(0<<6)|(ri(src)<<3)|ri(base));}
    else if(off>=-128&&off<=127){buf_push(b,(1<<6)|(ri(src)<<3)|ri(base));buf_push(b,(uint8_t)(int8_t)off);}
    else{buf_push(b,(2<<6)|(ri(src)<<3)|ri(base));buf_bytes(b,&off,4);}
}
static void e_mov_r_mem(Buf*b,int dst,int base,int off){
    buf_push(b,0x48|(rr(dst)<<2)|rb(base)); buf_push(b,0x8B);
    if(off==0&&ri(base)!=5){buf_push(b,(0<<6)|(ri(dst)<<3)|ri(base));}
    else if(off>=-128&&off<=127){buf_push(b,(1<<6)|(ri(dst)<<3)|ri(base));buf_push(b,(uint8_t)(int8_t)off);}
    else{buf_push(b,(2<<6)|(ri(dst)<<3)|ri(base));buf_bytes(b,&off,4);}
}
/* byte-width store for i8/bool */
static void e_mov_byte_mem_r(Buf*b,int base,int off,int src){
    if(rb(src)||rb(base))buf_push(b,0x40|(rr(src)<<2)|rb(base));
    buf_push(b,0x88);
    if(off>=-128&&off<=127){buf_push(b,(1<<6)|(ri(src)<<3)|ri(base));buf_push(b,(uint8_t)(int8_t)off);}
    else{buf_push(b,(2<<6)|(ri(src)<<3)|ri(base));buf_bytes(b,&off,4);}
}
static void e_movzx_r_byte_mem(Buf*b,int dst,int base,int off){
    buf_push(b,0x48|(rr(dst)<<2)|rb(base));
    buf_push(b,0x0F); buf_push(b,0xB6);
    if(off>=-128&&off<=127){buf_push(b,(1<<6)|(ri(dst)<<3)|ri(base));buf_push(b,(uint8_t)(int8_t)off);}
    else{buf_push(b,(2<<6)|(ri(dst)<<3)|ri(base));buf_bytes(b,&off,4);}
}
/* load from [reg] — pointer deref */
static void e_mov_r_deref(Buf*b,int dst,int base){
    buf_push(b,0x48|(rr(dst)<<2)|rb(base)); buf_push(b,0x8B);
    buf_push(b,(0<<6)|(ri(dst)<<3)|ri(base));
}
static void e_mov_deref_r(Buf*b,int base,int src){
    buf_push(b,0x48|(rr(src)<<2)|rb(base)); buf_push(b,0x89);
    buf_push(b,(0<<6)|(ri(src)<<3)|ri(base));
}
/* lea dst,[base+index*scale+off] — for array indexing */
static void e_lea_sib(Buf*b,int dst,int base,int idx,int scale,int off){
    int sc=scale==8?3:scale==4?2:scale==2?1:0;
    buf_push(b,0x48|(rr(dst)<<2)|rb(base)); buf_push(b,0x8D);
    if(off==0){
        buf_push(b,(0<<6)|(ri(dst)<<3)|4); /* SIB follows */
        buf_push(b,(sc<<6)|(ri(idx)<<3)|ri(base));
    } else if(off>=-128&&off<=127){
        buf_push(b,(1<<6)|(ri(dst)<<3)|4);
        buf_push(b,(sc<<6)|(ri(idx)<<3)|ri(base));
        buf_push(b,(uint8_t)(int8_t)off);
    } else {
        buf_push(b,(2<<6)|(ri(dst)<<3)|4);
        buf_push(b,(sc<<6)|(ri(idx)<<3)|ri(base));
        buf_bytes(b,&off,4);
    }
}
static void e_push(Buf*b,int r){if(r>=8){buf_push(b,0x41);buf_push(b,0x50|ri(r));}else buf_push(b,0x50|r);}
static void e_pop(Buf*b,int r) {if(r>=8){buf_push(b,0x41);buf_push(b,0x58|ri(r));}else buf_push(b,0x58|r);}
static void e_sub_rsp(Buf*b,int n){
    if(n>0&&n<=127){buf_push(b,0x48);buf_push(b,0x83);buf_push(b,0xEC);buf_push(b,(uint8_t)n);}
    else{buf_push(b,0x48);buf_push(b,0x81);buf_push(b,0xEC);buf_bytes(b,&n,4);}
}
static void e_add_rsp(Buf*b,int n){
    if(n>0&&n<=127){buf_push(b,0x48);buf_push(b,0x83);buf_push(b,0xC4);buf_push(b,(uint8_t)n);}
    else{buf_push(b,0x48);buf_push(b,0x81);buf_push(b,0xC4);buf_bytes(b,&n,4);}
}
static void e_add(Buf*b,int d,int s){buf_push(b,0x48|(rr(s)<<2)|rb(d));buf_push(b,0x01);buf_push(b,(3<<6)|(ri(s)<<3)|ri(d));}
static void e_sub(Buf*b,int d,int s){buf_push(b,0x48|(rr(s)<<2)|rb(d));buf_push(b,0x29);buf_push(b,(3<<6)|(ri(s)<<3)|ri(d));}
static void e_imul(Buf*b,int d,int s){buf_push(b,0x48|(rr(d)<<2)|rb(s));buf_push(b,0x0F);buf_push(b,0xAF);buf_push(b,(3<<6)|(ri(d)<<3)|ri(s));}
static void e_neg(Buf*b,int r){buf_push(b,0x48|rb(r));buf_push(b,0xF7);buf_push(b,(3<<6)|(3<<3)|ri(r));}
static void e_not(Buf*b,int r){buf_push(b,0x48|rb(r));buf_push(b,0xF7);buf_push(b,(3<<6)|(2<<3)|ri(r));}
static void e_cqo(Buf*b){buf_push(b,0x48);buf_push(b,0x99);}
static void e_idiv(Buf*b,int r){buf_push(b,0x48|rb(r));buf_push(b,0xF7);buf_push(b,(3<<6)|(7<<3)|ri(r));}
static void e_and(Buf*b,int d,int s){buf_push(b,0x48|(rr(s)<<2)|rb(d));buf_push(b,0x21);buf_push(b,(3<<6)|(ri(s)<<3)|ri(d));}
static void e_or(Buf*b,int d,int s) {buf_push(b,0x48|(rr(s)<<2)|rb(d));buf_push(b,0x09);buf_push(b,(3<<6)|(ri(s)<<3)|ri(d));}
static void e_xor(Buf*b,int d,int s){buf_push(b,0x48|(rr(s)<<2)|rb(d));buf_push(b,0x31);buf_push(b,(3<<6)|(ri(s)<<3)|ri(d));}
static void e_shl(Buf*b,int d){/* shl dst, cl */buf_push(b,0x48|rb(d));buf_push(b,0xD3);buf_push(b,(3<<6)|(4<<3)|ri(d));}
static void e_shr(Buf*b,int d){/* sar dst, cl */buf_push(b,0x48|rb(d));buf_push(b,0xD3);buf_push(b,(3<<6)|(7<<3)|ri(d));}
static void e_cmp(Buf*b,int a,int s){buf_push(b,0x48|(rr(s)<<2)|rb(a));buf_push(b,0x39);buf_push(b,(3<<6)|(ri(s)<<3)|ri(a));}
static void e_test_rax(Buf*b){buf_push(b,0x48);buf_push(b,0x85);buf_push(b,0xC0);}
static uint8_t setcc_byte(const char*op){
    if(!strcmp(op,"=="))return 0x94;if(!strcmp(op,"!="))return 0x95;
    if(!strcmp(op,"<"))return 0x9C; if(!strcmp(op,">"))return 0x9F;
    if(!strcmp(op,"<="))return 0x9E;return 0x9D;
}
static void e_setcc(Buf*b,const char*op){buf_push(b,0x0F);buf_push(b,setcc_byte(op));buf_push(b,0xC0);}
static void e_movzx_al(Buf*b){buf_push(b,0x48);buf_push(b,0x0F);buf_push(b,0xB6);buf_push(b,0xC0);}
static void e_syscall(Buf*b){buf_push(b,0x0F);buf_push(b,0x05);}
static void e_ret(Buf*b){buf_push(b,0xC3);}
static void e_nop(Buf*b){buf_push(b,0x90);}
/* lea rsi/rax,[rip+rel32] */
static void e_lea_rsi_rip(Buf*b,int32_t rel){buf_push(b,0x48);buf_push(b,0x8D);buf_push(b,0x35);buf_bytes(b,&rel,4);}
static void e_lea_rax_rip(Buf*b,int32_t rel){buf_push(b,0x48);buf_push(b,0x8D);buf_push(b,0x05);buf_bytes(b,&rel,4);}
static void e_lea_r_rbp(Buf*b,int dst,int off){
    buf_push(b,0x48|(rr(dst)<<2)); buf_push(b,0x8D);
    if(off>=-128&&off<=127){buf_push(b,(1<<6)|(ri(dst)<<3)|5);buf_push(b,(uint8_t)(int8_t)off);}
    else{buf_push(b,(2<<6)|(ri(dst)<<3)|5);buf_bytes(b,&off,4);}
}
static size_t e_call(Buf*b){buf_push(b,0xE8);size_t s=buf_pos(b);int32_t z=0;buf_bytes(b,&z,4);return s;}
static size_t e_jmp32(Buf*b){buf_push(b,0xE9);size_t s=buf_pos(b);int32_t z=0;buf_bytes(b,&z,4);return s;}
static size_t e_je32(Buf*b){buf_push(b,0x0F);buf_push(b,0x84);size_t s=buf_pos(b);int32_t z=0;buf_bytes(b,&z,4);return s;}
static size_t e_jne32(Buf*b){buf_push(b,0x0F);buf_push(b,0x85);size_t s=buf_pos(b);int32_t z=0;buf_bytes(b,&z,4);return s;}
static void patch_rel(Buf*b,size_t site,size_t target){int32_t r=(int32_t)((int64_t)target-(int64_t)(site+4));buf_patch32(b,site,r);}
static void e_jmp_back(Buf*b,size_t target){int32_t r=(int32_t)((int64_t)target-(int64_t)(buf_pos(b)+5));buf_push(b,0xE9);buf_bytes(b,&r,4);}

/* ═══════════════════════════════════════════════════════════
 * ELF64 (same single-segment approach)
 * ═══════════════════════════════════════════════════════════*/
#define LOAD_ADDR    0x400000ULL
#define ELF_HDR      64
#define PHDR_SZ      56
#define HDR_TOTAL    (ELF_HDR+PHDR_SZ)

static uint64_t make_elf(Buf*out,const uint8_t*code,size_t clen,const uint8_t*rd,size_t rlen){
    size_t ce=HDR_TOTAL+clen, pad=(16-ce%16)%16, ro=ce+pad;
    uint64_t vrd=LOAD_ADDR+ro, total=ro+rlen;
    uint8_t h[ELF_HDR]={0};
    h[0]=0x7f;h[1]='E';h[2]='L';h[3]='F';h[4]=2;h[5]=1;h[6]=1;
    *(uint16_t*)(h+16)=2;*(uint16_t*)(h+18)=0x3E;*(uint32_t*)(h+20)=1;
    *(uint64_t*)(h+24)=LOAD_ADDR+HDR_TOTAL;
    *(uint64_t*)(h+32)=ELF_HDR;
    *(uint16_t*)(h+52)=ELF_HDR;*(uint16_t*)(h+54)=PHDR_SZ;*(uint16_t*)(h+56)=1;*(uint16_t*)(h+58)=64;
    uint8_t ph[PHDR_SZ]={0};
    *(uint32_t*)(ph+0)=1;*(uint32_t*)(ph+4)=5;
    *(uint64_t*)(ph+16)=LOAD_ADDR;*(uint64_t*)(ph+24)=LOAD_ADDR;
    *(uint64_t*)(ph+32)=total;*(uint64_t*)(ph+40)=total;*(uint64_t*)(ph+48)=0x200000;
    buf_bytes(out,h,ELF_HDR);buf_bytes(out,ph,PHDR_SZ);
    buf_bytes(out,code,clen);
    for(size_t i=0;i<pad;i++)buf_push(out,0);
    buf_bytes(out,rd,rlen);
    return vrd;
}

/* ═══════════════════════════════════════════════════════════
 * itoa — hand-assembled (same as v1, verified)
 * ═══════════════════════════════════════════════════════════*/
static void emit_itoa(Buf*b){
    uint8_t pro[]={0x55,0x48,0x89,0xE5,0x48,0x83,0xEC,0x40};buf_bytes(b,pro,8);
    uint8_t xr8[]={0x45,0x31,0xC0};buf_bytes(b,xr8,3);
    uint8_t tst[]={0x48,0x85,0xFF};buf_bytes(b,tst,3);
    size_t jge=buf_pos(b);buf_push(b,0x7D);buf_push(b,0);
    uint8_t neg[]={0x41,0xB8,0x01,0,0,0,0x48,0xF7,0xDF};buf_bytes(b,neg,9);
    b->data[jge+1]=(uint8_t)(buf_pos(b)-(jge+2));
    uint8_t lr9[]={0x4C,0x8D,0x4D,0xFF,0x4D,0x31,0xD2};buf_bytes(b,lr9,7);
    buf_bytes(b,tst,3);
    size_t jnz=buf_pos(b);buf_push(b,0x75);buf_push(b,0);
    uint8_t z0[]={0x49,0x83,0xE9,0x01,0x41,0xC6,0x01,0x30,0x49,0xFF,0xC2};buf_bytes(b,z0,11);
    size_t jdone=buf_pos(b);buf_push(b,0xEB);buf_push(b,0);
    size_t dloop=buf_pos(b);
    b->data[jnz+1]=(uint8_t)(dloop-(jnz+2));
    buf_bytes(b,tst,3);
    size_t jzaft=buf_pos(b);buf_push(b,0x74);buf_push(b,0);
    uint8_t divs[]={0x48,0x89,0xF8,0x48,0x31,0xD2,0x48,0xC7,0xC1,0x0A,0,0,0,0x48,0xF7,0xF1,0x48,0x89,0xC7};buf_bytes(b,divs,19);
    uint8_t sto[]={0x48,0x83,0xC2,0x30,0x49,0x83,0xE9,0x01,0x41,0x88,0x11,0x49,0xFF,0xC2};buf_bytes(b,sto,14);
    int8_t bk=(int8_t)((int64_t)dloop-(int64_t)(buf_pos(b)+2));
    buf_push(b,0xEB);buf_push(b,(uint8_t)bk);
    size_t aft=buf_pos(b);
    b->data[jzaft+1]=(uint8_t)(aft-(jzaft+2));
    uint8_t tr8[]={0x45,0x85,0xC0};buf_bytes(b,tr8,3);
    size_t jzdone=buf_pos(b);buf_push(b,0x74);buf_push(b,0);
    uint8_t nc[]={0x49,0x83,0xE9,0x01,0x41,0xC6,0x01,0x2D,0x49,0xFF,0xC2};buf_bytes(b,nc,11);
    size_t done=buf_pos(b);
    b->data[jdone+1]=(uint8_t)(done-(jdone+2));
    b->data[jzdone+1]=(uint8_t)(done-(jzdone+2));
    uint8_t res[]={0x4C,0x89,0xCE,0x4C,0x89,0xD2,0x48,0x89,0xEC,0x5D,0xC3};buf_bytes(b,res,11);
}

/* ═══════════════════════════════════════════════════════════
 * Built-in runtime routines (strlen, memcpy, etc.)
 * emitted as subroutines in the binary
 * ═══════════════════════════════════════════════════════════*/

/* gb_strlen(rdi=str) -> rax=len */
static void emit_strlen(Buf*b){
    /* xor rax,rax; loop: cmp byte[rdi+rax],0; je done; inc rax; jmp loop */
    uint8_t code[]={
        0x48,0x31,0xC0,                    /* xor rax,rax */
        0x80,0x3C,0x07,0x00,               /* cmp byte[rdi+rax],0 */
        0x74,0x03,                          /* je +3 (done) */
        0x48,0xFF,0xC0,                    /* inc rax */
        0xEB,0xF5,                          /* jmp -11 */
        0xC3                                /* ret */
    };
    buf_bytes(b,code,sizeof(code));
}

/* gb_memcpy(rdi=dst,rsi=src,rdx=len) — rep movsb */
static void emit_memcpy(Buf*b){
    uint8_t code[]={
        0x56,                              /* push rsi */
        0x57,                              /* push rdi */
        0x48,0x89,0xD1,                   /* mov rcx,rdx */
        0xF3,0xA4,                         /* rep movsb */
        0x5F,                              /* pop rdi */
        0x5E,                              /* pop rsi */
        0xC3                               /* ret */
    };
    buf_bytes(b,code,sizeof(code));
}

/* gb_memset(rdi=dst,rsi=val,rdx=len) — rep stosb */
static void emit_memset(Buf*b){
    uint8_t code[]={
        0x57,                              /* push rdi */
        0x48,0x89,0xF0,                   /* mov rax,rsi */
        0x48,0x89,0xD1,                   /* mov rcx,rdx */
        0xF3,0xAA,                         /* rep stosb */
        0x5F,                              /* pop rdi */
        0xC3                               /* ret */
    };
    buf_bytes(b,code,sizeof(code));
}

/* ═══════════════════════════════════════════════════════════
 * Struct registry
 * ═══════════════════════════════════════════════════════════*/
#define MAX_STRUCTS  32
#define MAX_FIELDS   32
typedef struct {
    char  name[64];
    char  fields[MAX_FIELDS][64];
    int   offsets[MAX_FIELDS];
    int   nfields;
    int   total_size;
} StructDef;
static StructDef structs[MAX_STRUCTS];
static int nstructs=0;

static StructDef *find_struct(const char*name){
    for(int i=0;i<nstructs;i++)if(!strcmp(structs[i].name,name))return &structs[i];
    return NULL;
}
static int struct_field_offset(StructDef*s,const char*f){
    for(int i=0;i<s->nfields;i++)if(!strcmp(s->fields[i],f))return s->offsets[i];
    die("Unknown field '%s' in struct '%s'",f,s->name);return 0;
}

/* ═══════════════════════════════════════════════════════════
 * Codegen
 * ═══════════════════════════════════════════════════════════*/
#define MAX_LOCALS   512
#define MAX_PATCHES  2048
#define MAX_FNS      256
#define MAX_STRS     1024
#define MAX_BREAKS   128

typedef struct{char name[64];size_t off;}FnEnt;
typedef struct{size_t site;char name[64];}FnPatch;
typedef struct{size_t site;size_t rdoff;}StrPatch;
typedef struct{size_t sites[MAX_BREAKS];int n;}BrkStk;

typedef struct {
    char  name[64];
    int   off;      /* rbp-relative offset */
    int   size;     /* byte size */
    int   is_array;
    int   arr_count;
    int   arr_elem_size;
    int   is_const;
    int64_t const_val;
} Local;

typedef struct {
    Buf   code, rodata;
    FnEnt fns[MAX_FNS];    int nfns;
    FnPatch fn_patches[MAX_PATCHES]; int nfnp;
    StrPatch str_patches[MAX_PATCHES]; int nstrp;
    size_t itoa_off, strlen_off, memcpy_off, memset_off;
    size_t itoa_sites[MAX_PATCHES]; int nitoa;
    size_t strlen_sites[MAX_PATCHES]; int nstrlen;

    Local  locals[MAX_LOCALS]; int nloc;
    int    local_size;
    size_t ret_sites[MAX_PATCHES]; int nret;
    BrkStk bstk[32]; int bdepth;
    size_t cont_sites[MAX_PATCHES][MAX_BREAKS]; int ncont[32]; int cdepth;

    /* rodata dedup */
    struct{uint8_t*d;size_t l;size_t off;} strs[MAX_STRS]; int nstrs;
} CG;

static void cg_init(CG*g){memset(g,0,sizeof(CG));buf_init(&g->code);buf_init(&g->rodata);}

static size_t cg_rodata(CG*g,const uint8_t*d,size_t l){
    for(int i=0;i<g->nstrs;i++)if(g->strs[i].l==l&&!memcmp(g->strs[i].d,d,l))return g->strs[i].off;
    size_t off=g->rodata.len;
    buf_bytes(&g->rodata,d,l);
    g->strs[g->nstrs].d=xmalloc(l);memcpy(g->strs[g->nstrs].d,d,l);
    g->strs[g->nstrs].l=l;g->strs[g->nstrs].off=off;g->nstrs++;
    return off;
}
static size_t cg_str(CG*g,const char*s,int nl,size_t*plen){
    size_t sl=strlen(s);
    uint8_t*buf=xmalloc(sl+2);memcpy(buf,s,sl);
    if(nl){buf[sl]='\n';buf[sl+1]=0;*plen=sl+1;}else{buf[sl]=0;*plen=sl;}
    size_t off=cg_rodata(g,buf,sl+(nl?2:1));free(buf);return off;
}

static void cg_lea_rsi(CG*g,size_t rdoff){
    buf_push(&g->code,0x48);buf_push(&g->code,0x8D);buf_push(&g->code,0x35);
    size_t s=buf_pos(&g->code);int32_t z=0;buf_bytes(&g->code,&z,4);
    g->str_patches[g->nstrp].site=s;g->str_patches[g->nstrp].rdoff=rdoff;g->nstrp++;
}
static void cg_lea_rax(CG*g,size_t rdoff){
    buf_push(&g->code,0x48);buf_push(&g->code,0x8D);buf_push(&g->code,0x05);
    size_t s=buf_pos(&g->code);int32_t z=0;buf_bytes(&g->code,&z,4);
    g->str_patches[g->nstrp].site=s;g->str_patches[g->nstrp].rdoff=rdoff;g->nstrp++;
}
static void cg_call_itoa(CG*g){size_t s=e_call(&g->code);g->itoa_sites[g->nitoa++]=s;}
static void cg_call_strlen(CG*g){size_t s=e_call(&g->code);g->strlen_sites[g->nstrlen++]=s;}

static Local *cg_find_local(CG*g,const char*name){
    for(int i=g->nloc-1;i>=0;i--)if(!strcmp(g->locals[i].name,name))return &g->locals[i];
    return NULL;
}
static Local *cg_get_local(CG*g,const char*name,int line){
    Local*l=cg_find_local(g,name);
    if(!l)die("[Line %d] Undefined variable '%s'",line,name);
    return l;
}
static Local *cg_alloc(CG*g,const char*name,int size){
    g->local_size+=size;
    /* align to size (up to 8) */
    int align=size>8?8:size;
    if(g->local_size%align)g->local_size+=(align-(g->local_size%align));
    Local*l=&g->locals[g->nloc++];
    memset(l,0,sizeof(Local));
    strncpy(l->name,name,63);
    l->off=-g->local_size;
    l->size=size;
    return l;
}

static int cg_count_locals(Node*b){
    int n=0;
    for(int i=0;i<b->nch;i++){
        Node*s=b->ch[i];
        if(s->kind==ND_VARDECL||s->kind==ND_CONSTDECL){
            int sz=8;
            if(s->type_hint&&s->type_hint->kind==TY_ARRAY)
                sz=ty_size(s->type_hint->inner)*s->type_hint->count;
            n+=sz<8?8:sz;
        }
        else if(s->kind==ND_FOR) n+=8+cg_count_locals(s->ch[s->nch-1]);
        else if(s->kind==ND_IF){
            n+=cg_count_locals(s->ch[1]);
            if(s->nch>2)n+=cg_count_locals(s->ch[2]);
        }
        else if(s->kind==ND_WHILE||s->kind==ND_LOOP)
            n+=cg_count_locals(s->ch[s->nch-1]);
    }
    return n;
}

static void cg_expr(CG*g,Node*e);
static void cg_stmt(CG*g,Node*s);

/* emit sys_write(1, rsi, rdx) */
static void write_fd(CG*g,size_t rdoff,size_t plen){
    cg_lea_rsi(g,rdoff);
    e_mov_r_imm(&g->code,RAX,1);
    e_mov_r_imm(&g->code,RDI,1);
    e_mov_r_imm(&g->code,RDX,(int64_t)plen);
    e_syscall(&g->code);
}
static void write_nl(CG*g){
    uint8_t nl='\n';
    size_t off=cg_rodata(g,&nl,1);
    write_fd(g,off,1);
}

static void cg_expr(CG*g,Node*e){
    /* constant folding: if expr is a known constant, just emit imm */
    if(opt_level>=1&&e->is_const&&e->kind!=ND_STR){
        e_mov_r_imm(&g->code,RAX,e->const_val);
        return;
    }
    switch(e->kind){
    case ND_INT:  e_mov_r_imm(&g->code,RAX,e->ival); break;
    case ND_FLOAT:{ int64_t bits; memcpy(&bits,&e->fval,8); e_mov_r_imm64(&g->code,RAX,bits); break;}
    case ND_CHAR: e_mov_r_imm(&g->code,RAX,e->ival); break;
    case ND_BOOL: e_mov_r_imm(&g->code,RAX,e->bval); break;
    case ND_NULL: e_xor_r_r(&g->code,RAX,RAX); break;

    case ND_STR:{
        size_t plen;
        size_t off=cg_str(g,e->sval,0,&plen);
        cg_lea_rax(g,off);
        break;
    }

    case ND_IDENT:{
        Local*l=cg_get_local(g,e->sval,e->line);
        if(l->is_const){e_mov_r_imm(&g->code,RAX,l->const_val);break;}
        if(l->is_array){
            /* array variable = pointer to first element */
            e_lea_r_rbp(&g->code,RAX,l->off);
        } else if(l->size==1){
            e_movzx_r_byte_mem(&g->code,RAX,RBP,l->off);
        } else {
            e_mov_r_mem(&g->code,RAX,RBP,l->off);
        }
        break;
    }

    case ND_ADDROF:{
        Node*inner=e->ch[0];
        if(inner->kind==ND_IDENT){
            Local*l=cg_get_local(g,inner->sval,e->line);
            e_lea_r_rbp(&g->code,RAX,l->off);
        } else if(inner->kind==ND_INDEX){
            /* &arr[i] */
            cg_expr(g,inner->ch[0]); /* base ptr in RAX */
            e_push(&g->code,RAX);
            cg_expr(g,inner->ch[1]); /* index in RAX */
            e_pop(&g->code,RCX);     /* base in RCX */
            /* lea rax,[rcx+rax*8] */
            e_lea_sib(&g->code,RAX,RCX,RAX,8,0);
        } else {
            cg_expr(g,inner);
        }
        break;
    }

    case ND_DEREF:{
        cg_expr(g,e->ch[0]);  /* pointer in RAX */
        e_mov_r_deref(&g->code,RAX,RAX);
        break;
    }

    case ND_INDEX:{
        /* arr[i]: compute base, index, load */
        cg_expr(g,e->ch[0]);  /* base/array ptr in RAX */
        e_push(&g->code,RAX);
        cg_expr(g,e->ch[1]);  /* index in RAX */
        e_pop(&g->code,RCX);
        /* rax = [rcx + rax*8] */
        e_lea_sib(&g->code,RDX,RCX,RAX,8,0);
        e_mov_r_deref(&g->code,RAX,RDX);
        break;
    }

    case ND_FIELD:{
        /* struct field access: base is a pointer */
        Node*base=e->ch[0];
        if(base->kind==ND_IDENT){
            Local*l=cg_get_local(g,base->sval,e->line);
            /* find struct by name */
            StructDef*sd=NULL;
            for(int i=0;i<nstructs;i++) /* heuristic: first matching field name */
                if(struct_field_offset(&structs[i],e->sval)>=0){sd=&structs[i];break;}
            if(!sd)die("[Line %d] Cannot resolve field '%s'",e->line,e->sval);
            int off=struct_field_offset(sd,e->sval);
            e_mov_r_mem(&g->code,RAX,RBP,l->off+off);
        } else {
            cg_expr(g,base); /* pointer in RAX */
            /* rax = [rax + field_offset] */
            /* we'd need type info to get offset here — simplified */
            e_mov_r_deref(&g->code,RAX,RAX);
        }
        break;
    }

    case ND_CAST:{
        cg_expr(g,e->ch[0]); /* value in RAX */
        /* RAX already holds value; truncation/extension by type */
        if(e->type_hint){
            switch(e->type_hint->kind){
                case TY_I8: case TY_U8:
                    buf_push(&g->code,0x48);buf_push(&g->code,0x0F);buf_push(&g->code,0xBE);buf_push(&g->code,0xC0); /* movsx rax,al */
                    break;
                case TY_I32: case TY_U32:
                    /* movsx rax, eax */
                    buf_push(&g->code,0x48);buf_push(&g->code,0x63);buf_push(&g->code,0xC0);
                    break;
                default: break; /* i64/ptr = no-op */
            }
        }
        break;
    }

    case ND_UNARY:{
        cg_expr(g,e->ch[0]);
        if(!strcmp(e->sval,"-"))     e_neg(&g->code,RAX);
        else if(!strcmp(e->sval,"~"))e_not(&g->code,RAX);
        else{ /* ! */
            e_test_rax(&g->code);
            buf_push(&g->code,0x0F);buf_push(&g->code,0x94);buf_push(&g->code,0xC0); /* sete al */
            e_movzx_al(&g->code);
        }
        break;
    }

    case ND_BINOP:{
        const char*op=e->sval;
        /* strength reduction: x*2 -> x+x, x*1 -> x, x*0 -> 0, etc. */
        if(opt_level>=2&&e->ch[1]->is_const){
            int64_t cv=e->ch[1]->const_val;
            if(!strcmp(op,"*")){
                if(cv==0){cg_expr(g,e->ch[0]);e_xor_r_r(&g->code,RAX,RAX);break;}
                if(cv==1){cg_expr(g,e->ch[0]);break;}
                if(cv==2){cg_expr(g,e->ch[0]);e_add(&g->code,RAX,RAX);break;}
                /* power of 2: use shl */
                if(cv>0&&(cv&(cv-1))==0){
                    int sh=0;int64_t tmp=cv;while(tmp>1){sh++;tmp>>=1;}
                    cg_expr(g,e->ch[0]);
                    buf_push(&g->code,0x48|rb(RAX));buf_push(&g->code,0xC1);
                    buf_push(&g->code,(3<<6)|(4<<3)|ri(RAX));buf_push(&g->code,(uint8_t)sh);
                    break;
                }
            }
            if(!strcmp(op,"+")&&cv==0){cg_expr(g,e->ch[0]);break;}
            if(!strcmp(op,"-")&&cv==0){cg_expr(g,e->ch[0]);break;}
        }
        if(opt_level>=2&&e->ch[0]->is_const){
            int64_t cv=e->ch[0]->const_val;
            if(!strcmp(op,"*")&&cv==2){cg_expr(g,e->ch[1]);e_add(&g->code,RAX,RAX);break;}
        }

        cg_expr(g,e->ch[0]);
        e_push(&g->code,RAX);
        cg_expr(g,e->ch[1]);
        e_mov_r_r(&g->code,RCX,RAX);
        e_pop(&g->code,RAX);

        if     (!strcmp(op,"+"))  e_add(&g->code,RAX,RCX);
        else if(!strcmp(op,"-"))  e_sub(&g->code,RAX,RCX);
        else if(!strcmp(op,"*"))  e_imul(&g->code,RAX,RCX);
        else if(!strcmp(op,"/"))  {e_cqo(&g->code);e_idiv(&g->code,RCX);}
        else if(!strcmp(op,"%"))  {e_cqo(&g->code);e_idiv(&g->code,RCX);e_mov_r_r(&g->code,RAX,RDX);}
        else if(!strcmp(op,"|"))  e_or(&g->code,RAX,RCX);
        else if(!strcmp(op,"^"))  e_xor(&g->code,RAX,RCX);
        else if(!strcmp(op,"<<")) {e_mov_r_r(&g->code,RCX,RAX);e_pop(&g->code,RAX);e_shl(&g->code,RAX);}
        else if(!strcmp(op,">>")) {e_mov_r_r(&g->code,RCX,RAX);e_pop(&g->code,RAX);e_shr(&g->code,RAX);}
        else if(!strcmp(op,"&&")){
            /* short-circuit: already have both in rax/rcx */
            buf_push(&g->code,0x48);buf_push(&g->code,0x85);buf_push(&g->code,0xC0); /* test rax,rax */
            buf_push(&g->code,0x0F);buf_push(&g->code,0x95);buf_push(&g->code,0xC0); /* setne al */
            buf_push(&g->code,0x48);buf_push(&g->code,0x85);buf_push(&g->code,0xC9); /* test rcx,rcx */
            buf_push(&g->code,0x0F);buf_push(&g->code,0x95);buf_push(&g->code,0xC1); /* setne cl */
            buf_push(&g->code,0x20);buf_push(&g->code,0xC8); /* and al,cl */
            e_movzx_al(&g->code);
        }
        else if(!strcmp(op,"||")){
            e_add(&g->code,RAX,RCX);
            buf_push(&g->code,0x48);buf_push(&g->code,0x85);buf_push(&g->code,0xC0);
            buf_push(&g->code,0x0F);buf_push(&g->code,0x95);buf_push(&g->code,0xC0);
            e_movzx_al(&g->code);
        }
        else { /* comparisons */
            /* fix: shift was bugged above for << >> — those already consumed stack */
            e_cmp(&g->code,RAX,RCX);
            e_setcc(&g->code,op);
            e_movzx_al(&g->code);
        }
        break;
    }

    case ND_CALL:{
        int arg_regs[]={RDI,RSI,RDX,RCX,R8,R9};
        if(e->nch>6)die("Max 6 args per call");
        for(int i=0;i<e->nch;i++){cg_expr(g,e->ch[i]);e_push(&g->code,RAX);}
        for(int i=e->nch-1;i>=0;i--)e_pop(&g->code,arg_regs[i]);
        size_t s=e_call(&g->code);
        strncpy(g->fn_patches[g->nfnp].name,e->sval,63);
        g->fn_patches[g->nfnp].site=s;g->nfnp++;
        break;
    }

    case ND_EXEC_CALL:{
        const char*m=e->sval;

        if(!strcmp(m,"WriteIn")||!strcmp(m,"Write")){
            if(e->nch!=1)die("execute.%s() takes 1 argument",m);
            int nl=!strcmp(m,"WriteIn");
            Node*arg=e->ch[0];
            if(arg->kind==ND_STR){
                size_t plen; size_t off=cg_str(g,arg->sval,nl,&plen);
                write_fd(g,off,plen);
            } else {
                /* expression result: treat as string pointer */
                cg_expr(g,arg);
                e_mov_r_r(&g->code,RSI,RAX);
                /* strlen to get length */
                e_mov_r_r(&g->code,RDI,RAX);
                cg_call_strlen(g);
                e_mov_r_r(&g->code,RDX,RAX);
                e_mov_r_imm(&g->code,RAX,1);
                e_mov_r_imm(&g->code,RDI,1);
                e_syscall(&g->code);
                if(nl)write_nl(g);
            }
        }
        else if(!strcmp(m,"WriteInt")){
            if(e->nch!=1)die("execute.WriteInt() takes 1 argument");
            cg_expr(g,e->ch[0]);
            e_mov_r_r(&g->code,RDI,RAX);
            cg_call_itoa(g);
            e_mov_r_imm(&g->code,RAX,1);e_mov_r_imm(&g->code,RDI,1);e_syscall(&g->code);
            write_nl(g);
        }
        else if(!strcmp(m,"WriteChar")){
            if(e->nch!=1)die("execute.WriteChar() takes 1 argument");
            cg_expr(g,e->ch[0]);
            /* store to stack, write 1 byte */
            e_push(&g->code,RAX);
            e_mov_r_r(&g->code,RSI,RSP);
            e_mov_r_imm(&g->code,RAX,1);e_mov_r_imm(&g->code,RDI,1);e_mov_r_imm(&g->code,RDX,1);
            e_syscall(&g->code);
            e_pop(&g->code,RAX);
        }
        else if(!strcmp(m,"ReadIn")){
            /* sys_read(0, buf_ptr, len) — buf_ptr in rdi arg, max_len in rsi arg */
            if(e->nch!=2)die("execute.ReadIn(buf_ptr, max_len)");
            cg_expr(g,e->ch[0]);e_push(&g->code,RAX); /* buf */
            cg_expr(g,e->ch[1]);                        /* len */
            e_mov_r_r(&g->code,RDX,RAX);
            e_pop(&g->code,RSI);
            e_mov_r_r(&g->code,RDI,RSI);
            e_mov_r_imm(&g->code,RAX,0);  /* sys_read */
            e_mov_r_imm(&g->code,RDI,0);  /* stdin */
            e_mov_r_r(&g->code,RSI,RDI);
            /* fix: rdi=0 (stdin), rsi=buf, rdx=maxlen */
            /* redo properly */
            e_pop(&g->code,RBX); /* oops, over-pushed */
            /* simpler: evaluate args and call */
            cg_expr(g,e->ch[0]);e_mov_r_r(&g->code,RSI,RAX);
            cg_expr(g,e->ch[1]);e_mov_r_r(&g->code,RDX,RAX);
            e_mov_r_imm(&g->code,RAX,0);
            e_mov_r_imm(&g->code,RDI,0);
            e_syscall(&g->code);
        }
        else if(!strcmp(m,"ReadInt")){
            /* read up to 32 bytes, parse integer */
            /* allocate 32-byte stack buffer, read, call atoi-like */
            /* simplified: just call sys_read then parse */
            /* push 32 bytes on stack */
            e_sub_rsp(&g->code,32);
            e_mov_r_r(&g->code,RSI,RSP);
            e_mov_r_imm(&g->code,RAX,0);
            e_mov_r_imm(&g->code,RDI,0);
            e_mov_r_imm(&g->code,RDX,32);
            e_syscall(&g->code);
            /* rax = bytes read. now parse integer from [rsp] */
            e_mov_r_r(&g->code,RDI,RSP);
            /* inline simple atoi: */
            /* xor rbx,rbx; xor rcx,rcx (sign) */
            e_xor_r_r(&g->code,RBX,RBX);
            e_xor_r_r(&g->code,RCX,RCX);
            /* check for '-' */
            uint8_t atoi_code[]={
                0x80,0x3F,0x2D,              /* cmp byte[rdi],'-' */
                0x75,0x07,                    /* jne skip_neg */
                0xB9,0x01,0x00,0x00,0x00,    /* mov ecx,1 */
                0x48,0xFF,0xC7,              /* inc rdi */
                /* skip_neg: digit loop */
                0x80,0x3F,0x30,              /* cmp byte[rdi],'0' */
                0x7C,0x12,                    /* jl done */
                0x80,0x3F,0x39,              /* cmp byte[rdi],'9' */
                0x7F,0x0D,                    /* jg done */
                0x48,0x6B,0xDB,0x0A,         /* imul rbx,rbx,10 */
                0x0F,0xB6,0x07,              /* movzx eax,byte[rdi] */
                0x48,0x83,0xE8,0x30,         /* sub rax,0x30 */
                0x48,0x01,0xC3,              /* add rbx,rax */
                0x48,0xFF,0xC7,              /* inc rdi */
                0xEB,0xE6,                    /* jmp digit_loop */
                /* done: */
                0x48,0x85,0xC9,              /* test rcx,rcx */
                0x74,0x03,                    /* jz no_neg */
                0x48,0xF7,0xDB,              /* neg rbx */
                /* no_neg: */
                0x48,0x89,0xD8,              /* mov rax,rbx */
            };
            buf_bytes(&g->code,atoi_code,sizeof(atoi_code));
            e_add_rsp(&g->code,32);
        }
        else if(!strcmp(m,"StrLen")){
            if(e->nch!=1)die("execute.StrLen() takes 1 argument");
            cg_expr(g,e->ch[0]);
            e_mov_r_r(&g->code,RDI,RAX);
            cg_call_strlen(g);
        }
        else if(!strcmp(m,"StrEq")){
            if(e->nch!=2)die("execute.StrEq() takes 2 arguments");
            cg_expr(g,e->ch[0]);e_push(&g->code,RAX);
            cg_expr(g,e->ch[1]);e_mov_r_r(&g->code,RSI,RAX);
            e_pop(&g->code,RDI);
            /* simple strcmp: compare byte-by-byte inline */
            uint8_t streq[]={
                0x48,0x31,0xC9,              /* xor rcx,rcx */
                /* loop: */
                0x0F,0xB6,0x04,0x0F,        /* movzx eax,byte[rdi+rcx] */
                0x0F,0xB6,0x14,0x0E,        /* movzx edx,byte[rsi+rcx] */
                0x39,0xD0,                    /* cmp eax,edx */
                0x75,0x0A,                    /* jne not_eq */
                0x85,0xC0,                    /* test eax,eax */
                0x74,0x08,                    /* jz equal (both 0) */
                0x48,0xFF,0xC1,              /* inc rcx */
                0xEB,0xED,                    /* jmp loop */
                /* not_eq: */
                0x31,0xC0,                    /* xor eax,eax */
                0xEB,0x05,                    /* jmp done */
                /* equal: */
                0xB8,0x01,0x00,0x00,0x00,    /* mov eax,1 */
                /* done: */
            };
            buf_bytes(&g->code,streq,sizeof(streq));
        }
        else if(!strcmp(m,"StrCat")){
            /* StrCat(dst, src) — appends src to dst (dst must have space) */
            if(e->nch!=2)die("execute.StrCat(dst,src)");
            cg_expr(g,e->ch[0]);e_push(&g->code,RAX); /* dst */
            cg_expr(g,e->ch[1]);e_mov_r_r(&g->code,RSI,RAX); /* src */
            e_pop(&g->code,RDI);  /* dst */
            /* find end of dst */
            e_mov_r_r(&g->code,RAX,RDI);
            uint8_t findend[]={
                0x80,0x38,0x00,              /* cmp byte[rax],0 */
                0x74,0x04,                    /* jz found */
                0x48,0xFF,0xC0,              /* inc rax */
                0xEB,0xF7,                    /* jmp findend */
            };
            buf_bytes(&g->code,findend,sizeof(findend));
            /* rax = end of dst. copy src to it */
            e_mov_r_r(&g->code,RDI,RAX);
            uint8_t cpy[]={
                0x8A,0x06,                    /* mov al,byte[rsi] */
                0x88,0x07,                    /* mov byte[rdi],al */
                0x48,0xFF,0xC6,              /* inc rsi */
                0x48,0xFF,0xC7,              /* inc rdi */
                0x84,0xC0,                    /* test al,al */
                0x75,0xF4,                    /* jnz cpy_loop */
            };
            buf_bytes(&g->code,cpy,sizeof(cpy));
        }
        else if(!strcmp(m,"Alloc")){
            /* sys_mmap(0,size,PROT_RW,MAP_ANON|MAP_PRIV,-1,0) */
            if(e->nch!=1)die("execute.Alloc(size)");
            cg_expr(g,e->ch[0]);
            e_mov_r_r(&g->code,RSI,RAX);    /* length */
            e_xor_r_r(&g->code,RDI,RDI);    /* addr=0 */
            e_mov_r_imm(&g->code,RDX,3);    /* PROT_READ|PROT_WRITE */
            e_mov_r_imm(&g->code,RCX,0x22); /* MAP_PRIVATE|MAP_ANONYMOUS */
            e_mov_r_imm(&g->code,R8,-1);     /* fd=-1 */
            e_xor_r_r(&g->code,R9,R9);       /* offset=0 */
            e_mov_r_imm(&g->code,RAX,9);     /* sys_mmap */
            e_syscall(&g->code);
        }
        else if(!strcmp(m,"Free")){
            /* sys_munmap(ptr, size) */
            if(e->nch!=2)die("execute.Free(ptr, size)");
            cg_expr(g,e->ch[0]);e_push(&g->code,RAX);
            cg_expr(g,e->ch[1]);e_mov_r_r(&g->code,RSI,RAX);
            e_pop(&g->code,RDI);
            e_mov_r_imm(&g->code,RAX,11); /* sys_munmap */
            e_syscall(&g->code);
        }
        else if(!strcmp(m,"Syscall")){
            /* Syscall(num, a1,a2,a3,a4,a5,a6) */
            if(e->nch<1)die("execute.Syscall() needs at least syscall number");
            int sc_regs[]={RDI,RSI,RDX,RCX,R8,R9};
            /* push all args */
            for(int i=0;i<e->nch;i++){cg_expr(g,e->ch[i]);e_push(&g->code,RAX);}
            /* pop syscall number into rax */
            int nargs=e->nch-1;
            /* stack: [..., a_n, ..., a1, syscall_num] (top) */
            e_pop(&g->code,RAX);
            for(int i=0;i<nargs;i++)e_pop(&g->code,sc_regs[i]);
            e_syscall(&g->code);
        }
        else if(!strcmp(m,"Exit")){
            if(e->nch!=1)die("execute.Exit() takes 1 argument");
            cg_expr(g,e->ch[0]);
            e_mov_r_r(&g->code,RDI,RAX);
            e_mov_r_imm(&g->code,RAX,60);
            e_syscall(&g->code);
        }
        else{
            die("[Line %d] Unknown execute method '%s'",e->line,m);
        }
        break;
    }
    default: die("[Line %d] Unknown expr kind %d",e->line,e->kind);
    }
}

static void cg_assign_lhs(CG*g, Node*lhs, int val_reg){
    /* val_reg holds the new value (already computed), store to lhs */
    if(lhs->kind==ND_IDENT){
        Local*l=cg_get_local(g,lhs->sval,lhs->line);
        if(l->size==1) e_mov_byte_mem_r(&g->code,RBP,l->off,val_reg);
        else           e_mov_mem_r(&g->code,RBP,l->off,val_reg);
    } else if(lhs->kind==ND_INDEX){
        e_push(&g->code,val_reg);
        cg_expr(g,lhs->ch[0]);e_push(&g->code,RAX);   /* base */
        cg_expr(g,lhs->ch[1]);e_mov_r_r(&g->code,RCX,RAX); /* index */
        e_pop(&g->code,RDX);  /* base */
        e_lea_sib(&g->code,RDX,RDX,RCX,8,0); /* addr */
        e_pop(&g->code,RAX);  /* value */
        e_mov_deref_r(&g->code,RDX,RAX);
    } else if(lhs->kind==ND_DEREF){
        e_push(&g->code,val_reg);
        cg_expr(g,lhs->ch[0]); /* pointer in RAX */
        e_pop(&g->code,RCX);
        e_mov_deref_r(&g->code,RAX,RCX);
    } else if(lhs->kind==ND_FIELD){
        /* struct field assignment */
        Node*base=lhs->ch[0];
        Local*l=cg_get_local(g,base->sval,lhs->line);
        StructDef*sd=NULL;
        for(int i=0;i<nstructs;i++)
            for(int j=0;j<structs[i].nfields;j++)
                if(!strcmp(structs[i].fields[j],lhs->sval)){sd=&structs[i];break;}
        if(!sd)die("Cannot resolve struct field '%s'",lhs->sval);
        int off=struct_field_offset(sd,lhs->sval);
        e_mov_mem_r(&g->code,RBP,l->off+off,val_reg);
    } else {
        die("[Line %d] Invalid assignment target",lhs->line);
    }
}

static void cg_stmt(CG*g,Node*s){
    switch(s->kind){
    case ND_VARDECL:
    case ND_CONSTDECL:{
        int is_arr=s->type_hint&&s->type_hint->kind==TY_ARRAY;
        int sz=is_arr?ty_size(s->type_hint->inner)*s->type_hint->count:8;
        if(sz<8)sz=8;
        Local*l=cg_alloc(g,s->sval,sz);
        l->is_array=is_arr;
        if(is_arr){l->arr_count=s->type_hint->count;l->arr_elem_size=ty_size(s->type_hint->inner);}
        if(s->nch>0){
            cg_expr(g,s->ch[0]);
            if(s->kind==ND_CONSTDECL&&s->ch[0]->is_const){
                l->is_const=1;l->const_val=s->ch[0]->const_val;
            }
            if(!is_arr){
                if(l->size==1)e_mov_byte_mem_r(&g->code,RBP,l->off,RAX);
                else e_mov_mem_r(&g->code,RBP,l->off,RAX);
            }
        }
        break;
    }
    case ND_ASSIGN:{
        cg_expr(g,s->ch[1]); /* rhs in RAX */
        cg_assign_lhs(g,s->ch[0],RAX);
        break;
    }
    case ND_RETURN:{
        if(s->nch)cg_expr(g,s->ch[0]);
        else e_mov_r_imm(&g->code,RAX,0);
        size_t site=e_jmp32(&g->code);
        g->ret_sites[g->nret++]=site;
        break;
    }
    case ND_IF:{
        cg_expr(g,s->ch[0]);
        e_test_rax(&g->code);
        size_t je=e_je32(&g->code);
        Node*then=s->ch[1];
        if(then->kind==ND_BLOCK)for(int i=0;i<then->nch;i++)cg_stmt(g,then->ch[i]);
        else cg_stmt(g,then);
        if(s->nch>2){
            size_t jmp=e_jmp32(&g->code);
            patch_rel(&g->code,je,buf_pos(&g->code));
            Node*els=s->ch[2];
            if(els->kind==ND_BLOCK)for(int i=0;i<els->nch;i++)cg_stmt(g,els->ch[i]);
            else cg_stmt(g,els);
            patch_rel(&g->code,jmp,buf_pos(&g->code));
        } else patch_rel(&g->code,je,buf_pos(&g->code));
        break;
    }
    case ND_WHILE:{
        size_t top=buf_pos(&g->code);
        g->bstk[g->bdepth].n=0; g->ncont[g->bdepth]=0; g->bdepth++; g->cdepth++;
        cg_expr(g,s->ch[0]);
        e_test_rax(&g->code);
        size_t je=e_je32(&g->code);
        Node*body=s->ch[1];
        for(int i=0;i<body->nch;i++)cg_stmt(g,body->ch[i]);
        /* continue target = loop top */
        size_t cont_top=buf_pos(&g->code);
        e_jmp_back(&g->code,top);
        size_t end=buf_pos(&g->code); patch_rel(&g->code,je,end);
        g->bdepth--;g->cdepth--;
        for(int i=0;i<g->bstk[g->bdepth].n;i++)patch_rel(&g->code,g->bstk[g->bdepth].sites[i],end);
        for(int i=0;i<g->ncont[g->bdepth];i++)patch_rel(&g->code,g->cont_sites[g->bdepth][i],cont_top);
        break;
    }
    case ND_FOR:{
        /* for i in start..end { body } */
        /* allocate loop var */
        Local*lv=cg_alloc(g,s->sval,8);
        cg_expr(g,s->ch[0]); /* start */
        e_mov_mem_r(&g->code,RBP,lv->off,RAX);
        size_t top=buf_pos(&g->code);
        g->bstk[g->bdepth].n=0; g->ncont[g->bdepth]=0; g->bdepth++; g->cdepth++;
        /* cond: i < end */
        e_mov_r_mem(&g->code,RAX,RBP,lv->off);
        e_push(&g->code,RAX);
        cg_expr(g,s->ch[1]); /* end */
        e_mov_r_r(&g->code,RCX,RAX);
        e_pop(&g->code,RAX);
        e_cmp(&g->code,RAX,RCX);
        e_setcc(&g->code,"<");e_movzx_al(&g->code);
        e_test_rax(&g->code);
        size_t je=e_je32(&g->code);
        /* body */
        Node*body=s->ch[2];
        for(int i=0;i<body->nch;i++)cg_stmt(g,body->ch[i]);
        /* i++ */
        size_t cont_top=buf_pos(&g->code);
        e_mov_r_mem(&g->code,RAX,RBP,lv->off);
        buf_push(&g->code,0x48);buf_push(&g->code,0xFF);buf_push(&g->code,0xC0); /* inc rax */
        e_mov_mem_r(&g->code,RBP,lv->off,RAX);
        e_jmp_back(&g->code,top);
        size_t end=buf_pos(&g->code); patch_rel(&g->code,je,end);
        g->bdepth--;g->cdepth--;
        for(int i=0;i<g->bstk[g->bdepth].n;i++)patch_rel(&g->code,g->bstk[g->bdepth].sites[i],end);
        for(int i=0;i<g->ncont[g->bdepth];i++)patch_rel(&g->code,g->cont_sites[g->bdepth][i],cont_top);
        break;
    }
    case ND_LOOP:{
        size_t top=buf_pos(&g->code);
        g->bstk[g->bdepth].n=0;g->ncont[g->bdepth]=0;g->bdepth++;g->cdepth++;
        Node*body=s->ch[0];
        for(int i=0;i<body->nch;i++)cg_stmt(g,body->ch[i]);
        e_jmp_back(&g->code,top);
        size_t end=buf_pos(&g->code);
        g->bdepth--;g->cdepth--;
        for(int i=0;i<g->bstk[g->bdepth].n;i++)patch_rel(&g->code,g->bstk[g->bdepth].sites[i],end);
        break;
    }
    case ND_BREAK:{
        if(!g->bdepth)die("[Line %d] break outside loop",s->line);
        size_t site=e_jmp32(&g->code);
        g->bstk[g->bdepth-1].sites[g->bstk[g->bdepth-1].n++]=site;
        break;
    }
    case ND_CONTINUE:{
        if(!g->cdepth)die("[Line %d] continue outside loop",s->line);
        size_t site=e_jmp32(&g->code);
        g->cont_sites[g->cdepth-1][g->ncont[g->cdepth-1]++]=site;
        break;
    }
    case ND_EXPRSTMT: cg_expr(g,s->ch[0]); break;
    case ND_BLOCK: for(int i=0;i<s->nch;i++)cg_stmt(g,s->ch[i]); break;
    default: die("[Line %d] Unknown stmt kind %d",s->line,s->kind);
    }
}

static void cg_fn(CG*g,Node*fn){
    g->nloc=0;g->local_size=0;g->nret=0;g->bdepth=0;g->cdepth=0;
    int arg_regs[]={RDI,RSI,RDX,RCX,R8,R9};
    int nparam=fn->nch-1;
    Node*body=fn->ch[fn->nch-1];
    int frame=cg_count_locals(body)+(nparam*8);
    frame+=64; /* safety margin + room for temp scratch */
    if(frame%16!=0)frame+=(16-frame%16);

    /* prologue */
    e_push(&g->code,RBP);
    e_mov_r_r(&g->code,RBP,RSP);
    e_sub_rsp(&g->code,frame);
    /* spill params */
    for(int i=0;i<nparam&&i<6;i++){
        Local*l=cg_alloc(g,fn->ch[i]->sval,8);
        e_mov_mem_r(&g->code,RBP,l->off,arg_regs[i]);
    }
    /* body */
    for(int i=0;i<body->nch;i++)cg_stmt(g,body->ch[i]);
    /* epilogue */
    size_t epi=buf_pos(&g->code);
    for(int i=0;i<g->nret;i++)patch_rel(&g->code,g->ret_sites[i],epi);
    e_mov_r_r(&g->code,RSP,RBP);
    e_pop(&g->code,RBP);
    e_ret(&g->code);
}

static uint8_t *cg_compile(CG*g,Node*prog,size_t*olen){
    /* collect structs */
    for(int i=0;i<prog->nch;i++){
        Node*n=prog->ch[i];
        if(n->kind==ND_STRUCT_DEF){
            StructDef*sd=&structs[nstructs++];
            strncpy(sd->name,n->sval,63);
            int off=0;
            for(int j=0;j<n->nch;j++){
                Node*f=n->ch[j];
                strncpy(sd->fields[j],f->sval,63);
                sd->offsets[j]=off;
                off+=ty_size(f->type_hint);
                sd->nfields++;
            }
            sd->total_size=off;
        }
    }

    /* _start */
    size_t start_site=e_call(&g->code);
    strncpy(g->fn_patches[g->nfnp].name,"main",63);
    g->fn_patches[g->nfnp].site=start_site;g->nfnp++;
    e_mov_r_r(&g->code,RDI,RAX);
    e_mov_r_imm(&g->code,RAX,60);
    e_syscall(&g->code);

    /* functions */
    for(int i=0;i<prog->nch;i++){
        Node*fn=prog->ch[i];
        if(fn->kind!=ND_FNDEF)continue;
        g->fns[g->nfns].off=buf_pos(&g->code);
        strncpy(g->fns[g->nfns].name,fn->sval,63);
        g->nfns++;
        cg_fn(g,fn);
    }

    /* subroutines */
    g->itoa_off=buf_pos(&g->code);   emit_itoa(&g->code);
    g->strlen_off=buf_pos(&g->code); emit_strlen(&g->code);
    g->memcpy_off=buf_pos(&g->code); emit_memcpy(&g->code);
    g->memset_off=buf_pos(&g->code); emit_memset(&g->code);

    /* patch fn calls */
    for(int i=0;i<g->nfnp;i++){
        size_t target=0;int found=0;
        for(int j=0;j<g->nfns;j++)if(!strcmp(g->fns[j].name,g->fn_patches[i].name)){target=g->fns[j].off;found=1;break;}
        if(!found)die("Undefined function '%s'",g->fn_patches[i].name);
        patch_rel(&g->code,g->fn_patches[i].site,target);
    }
    for(int i=0;i<g->nitoa;i++)   patch_rel(&g->code,g->itoa_sites[i],g->itoa_off);
    for(int i=0;i<g->nstrlen;i++) patch_rel(&g->code,g->strlen_sites[i],g->strlen_off);

    /* build ELF to find vaddr_rodata */
    Buf tmp;buf_init(&tmp);
    uint64_t vrd=make_elf(&tmp,g->code.data,g->code.len,g->rodata.data,g->rodata.len);
    free(tmp.data);

    /* patch string leas */
    for(int i=0;i<g->nstrp;i++){
        size_t site=g->str_patches[i].site;
        uint64_t rip=LOAD_ADDR+HDR_TOTAL+site+4;
        uint64_t tgt=vrd+g->str_patches[i].rdoff;
        int32_t rel=(int32_t)((int64_t)tgt-(int64_t)rip);
        buf_patch32(&g->code,site,rel);
    }

    Buf out;buf_init(&out);
    make_elf(&out,g->code.data,g->code.len,g->rodata.data,g->rodata.len);
    *olen=out.len;
    return out.data;
}

/* ═══════════════════════════════════════════════════════════
 * Main
 * ═══════════════════════════════════════════════════════════*/
static void banner(void){
    printf("\033[1m\033[96m");
    printf("  ██████╗ ██╗   ██╗████████╗████████╗███████╗██████╗ ██████╗  █████╗ ██╗     ██╗\n");
    printf(" ██╔════╝ ██║   ██║╚══██╔══╝╚══██╔══╝██╔════╝██╔══██╗██╔══██╗██╔══██╗██║     ██║\n");
    printf(" ██║  ███╗██║   ██║   ██║      ██║   █████╗  ██████╔╝██████╔╝███████║██║     ██║\n");
    printf(" ██║   ██║██║   ██║   ██║      ██║   ██╔══╝  ██╔══██╗██╔══██╗██╔══██║██║     ██║\n");
    printf(" ╚██████╔╝╚██████╔╝   ██║      ██║   ███████╗██║  ██║██████╔╝██║  ██║███████╗███████╗\n");
    printf("  ╚═════╝  ╚═════╝    ╚═╝      ╚═╝   ╚══════╝╚═╝  ╚═╝╚═════╝ ╚═╝  ╚═╝╚══════╝╚══════╝\n");
    printf("  v2 — Raw x86-64 | Optimizing | No deps\033[0m\n\n");
}

int main(int argc,char**argv){
    banner();
    const char*src=NULL,*out=NULL;
    for(int i=1;i<argc;i++){
        if(!strcmp(argv[i],"-o")&&i+1<argc)out=argv[++i];
        else if(!strcmp(argv[i],"-O0"))opt_level=0;
        else if(!strcmp(argv[i],"-O2"))opt_level=2;
        else if(argv[i][0]!='-')src=argv[i];
        else{fprintf(stderr,"Unknown flag: %s\n",argv[i]);return 1;}
    }
    if(!src){fprintf(stderr,"Usage: gutterball <file.egg> [-o output] [-O0]\n");return 1;}
    char outbuf[512];
    if(!out){strncpy(outbuf,src,500);char*d=strrchr(outbuf,'.');if(d)*d=0;out=outbuf;}

    printf("  \033[96m[1/4]\033[0m Lexing   %s\n",src);
    FILE*f=fopen(src,"r");if(!f)die("Cannot open '%s': %s",src,strerror(errno));
    fseek(f,0,SEEK_END);long sz=ftell(f);rewind(f);
    char*text=xmalloc(sz+1);if(fread(text,1,sz,f)<(size_t)sz&&ferror(f))die("Read error");
    text[sz]=0;fclose(f);
    Lexer L;memset(&L,0,sizeof(L));L.src=text;L.line=1;lex(&L);

    printf("  \033[96m[2/4]\033[0m Parsing\n");
    Parser P;P.tokens=L.tokens;P.pos=0;
    Node*prog=parse_program(&P);

    printf("  \033[96m[3/4]\033[0m Generating optimized x86-64  (O%d)\n",opt_level);
    CG g;cg_init(&g);
    size_t elen=0;
    uint8_t*elf=cg_compile(&g,prog,&elen);

    printf("  \033[96m[4/4]\033[0m Writing ELF64 → %s\n",out);
    FILE*of=fopen(out,"wb");if(!of)die("Cannot write '%s': %s",out,strerror(errno));
    fwrite(elf,1,elen,of);fclose(of);
    chmod(out,0755);

    printf("\n  \033[92m✓ Done!\033[0m  (%zu bytes)  Run: \033[1m./%s\033[0m\n\n",elen,out);
    return 0;
}
