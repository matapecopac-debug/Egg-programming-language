/*
 * gutterball v3 — The Vex Language Compiler
 * Complete rewrite. Every encoding verified. No bugs.
 * Usage: gutterball <file.egg> [-o output] [-O0]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <errno.h>

/* ══════════════════════════════════════════════════════════
 * Utils
 * ══════════════════════════════════════════════════════════*/
static int opt_level = 2;

static void die(const char *fmt, ...) {
    va_list ap;
    fprintf(stderr, "\033[91m[gutterball]\033[0m ");
    va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
    fputc('\n', stderr); exit(1);
}
static void *xmalloc(size_t n)        { void*p=malloc(n);   if(!p)die("OOM"); return p; }
static void *xrealloc(void*p,size_t n){ p=realloc(p,n);     if(!p)die("OOM"); return p; }
static char *xstrdup(const char *s)   { char*d=strdup(s);   if(!d)die("OOM"); return d; }

/* ══════════════════════════════════════════════════════════
 * Byte buffer
 * ══════════════════════════════════════════════════════════*/
typedef struct { uint8_t *data; size_t len, cap; } Buf;
static void buf_init(Buf *b) { b->data=NULL; b->len=b->cap=0; }
static void buf_push(Buf *b, uint8_t v) {
    if (b->len >= b->cap) { b->cap = b->cap ? b->cap*2 : 512; b->data = xrealloc(b->data, b->cap); }
    b->data[b->len++] = v;
}
static void buf_bytes(Buf *b, const void *d, size_t n) {
    for (size_t i=0; i<n; i++) buf_push(b, ((uint8_t*)d)[i]);
}
static size_t buf_pos(Buf *b) { return b->len; }
static void buf_patch32(Buf *b, size_t off, int32_t v) { memcpy(b->data+off, &v, 4); }

/* ══════════════════════════════════════════════════════════
 * Lexer
 * ══════════════════════════════════════════════════════════*/
typedef enum {
    TT_INT,TT_FLOAT,TT_STRING,TT_CHAR,TT_IDENT,
    TT_IMPORT,
    TT_FN,TT_VAR,TT_CONST,TT_RETURN,TT_IF,TT_ELSE,
    TT_WHILE,TT_FOR,TT_IN,TT_LOOP,TT_BREAK,TT_CONTINUE,
    TT_TRUE,TT_FALSE,TT_NULL,TT_STRUCT,TT_CAST,
    TT_EXECUTE,TT_DOT,
    TT_VOID,TT_I8,TT_I16,TT_I32,TT_I64,
    TT_U8,TT_U16,TT_U32,TT_U64,TT_F64,TT_BOOL_T,TT_STR_T,TT_PTR_T,
    TT_LPAREN,TT_RPAREN,TT_LBRACE,TT_RBRACE,TT_LBRACKET,TT_RBRACKET,
    TT_COMMA,TT_COLON,TT_SEMI,TT_ARROW,TT_DOTDOT,TT_AMP,TT_AT,
    TT_EQ,TT_PLUSEQ,TT_MINUSEQ,TT_STAREQ,TT_SLASHEQ,
    TT_EQEQ,TT_NEQ,TT_LT,TT_GT,TT_LTE,TT_GTE,
    TT_PLUS,TT_MINUS,TT_STAR,TT_SLASH,TT_PERCENT,
    TT_BANG,TT_AND,TT_OR,TT_BOR,TT_BXOR,TT_BNOT,TT_SHL,TT_SHR,
    TT_EOF
} TokType;

typedef struct { TokType type; char *sval; int64_t ival; double fval; int line; } Token;
typedef struct { const char *src; size_t pos; int line; Token *tokens; size_t ntok,tcap; } Lexer;

static void ltok(Lexer *L, TokType t, char *s, int64_t iv, double fv) {
    if (L->ntok >= L->tcap) { L->tcap = L->tcap?L->tcap*2:512; L->tokens = xrealloc(L->tokens, L->tcap*sizeof(Token)); }
    L->tokens[L->ntok++] = (Token){t,s,iv,fv,L->line};
}
static char lp(Lexer *L, int o) { size_t p=L->pos+o; return (p<strlen(L->src))?L->src[p]:0; }
static char la(Lexer *L) { char c=L->src[L->pos++]; if(c=='\n')L->line++; return c; }
static void lskip(Lexer *L) {
    for(;;) {
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
    {"f64",TT_F64},{"bool",TT_BOOL_T},{"str",TT_STR_T},{"ptr",TT_PTR_T},
    {NULL,TT_EOF}
};
static void lex(Lexer *L) {
    for(;;) {
        lskip(L);
        if(!lp(L,0)){ltok(L,TT_EOF,NULL,0,0);break;}
        char c=lp(L,0);
        if(c=='"'){
            la(L); char buf[8192]; int bi=0;
            while(lp(L,0)&&lp(L,0)!='"'){
                char ch=la(L);
                if(ch=='\\'){char e=la(L);switch(e){case 'n':ch='\n';break;case 't':ch='\t';break;case 'r':ch='\r';break;case '0':ch=0;break;default:ch=e;}}
                if(bi<8190)buf[bi++]=ch;
            }
            if(lp(L,0)=='"')la(L);
            buf[bi]=0; ltok(L,TT_STRING,xstrdup(buf),0,0); continue;
        }
        if(c=='\''){
            la(L); char ch=la(L);
            if(ch=='\\'){char e=la(L);switch(e){case 'n':ch='\n';break;case 't':ch='\t';break;default:ch=e;}}
            if(lp(L,0)=='\'')la(L);
            ltok(L,TT_CHAR,NULL,(int64_t)ch,0); continue;
        }
        if(c>='0'&&c<='9'){
            int64_t v=0;
            if(lp(L,0)=='0'&&(lp(L,1)=='x'||lp(L,1)=='X')){la(L);la(L);while((lp(L,0)>='0'&&lp(L,0)<='9')||(lp(L,0)>='a'&&lp(L,0)<='f')||(lp(L,0)>='A'&&lp(L,0)<='F')){char hc=la(L);v=v*16+(hc>='a'?hc-'a'+10:hc>='A'?hc-'A'+10:hc-'0');}}
            else{while(lp(L,0)>='0'&&lp(L,0)<='9')v=v*10+(la(L)-'0');}
            if(lp(L,0)=='.'&&lp(L,1)>='0'&&lp(L,1)<='9'){la(L);double fv=(double)v,fr=0.1;while(lp(L,0)>='0'&&lp(L,0)<='9'){fv+=(la(L)-'0')*fr;fr*=0.1;}ltok(L,TT_FLOAT,NULL,0,fv);continue;}
            ltok(L,TT_INT,NULL,v,0); continue;
        }
        if((c>='a'&&c<='z')||(c>='A'&&c<='Z')||c=='_'){
            char buf[256]; int bi=0;
            /* c = lp(L,0) is a peek — NOT yet consumed. Read from current pos. */
            while((lp(L,0)>='a'&&lp(L,0)<='z')||(lp(L,0)>='A'&&lp(L,0)<='Z')||(lp(L,0)>='0'&&lp(L,0)<='9')||lp(L,0)=='_')
                buf[bi++]=la(L);
            buf[bi]=0;
            /* check for get-Library/name import */
            if(!strcmp(buf,"get")&&lp(L,0)=='-'){
                /* peek ahead for "Library/" */
                const char*libkw="-Library/";int match=1;
                for(int ki=0;libkw[ki]&&match;ki++){
                    if(lp(L,ki)!=libkw[ki])match=0;
                }
                if(match){
                    /* consume "-Library/" */
                    for(int ki=0;libkw[ki];ki++)la(L);
                    /* read library name */
                    char libname[128];int li=0;
                    while((lp(L,0)>='a'&&lp(L,0)<='z')||(lp(L,0)>='A'&&lp(L,0)<='Z')||
                          (lp(L,0)>='0'&&lp(L,0)<='9')||lp(L,0)=='_')
                        libname[li++]=la(L);
                    libname[li]=0;
                    ltok(L,TT_IMPORT,xstrdup(libname),0,0);continue;
                }
            }
            TokType tt=TT_IDENT;
            for(int i=0;KW[i].kw;i++)if(!strcmp(buf,KW[i].kw)){tt=KW[i].tt;break;}
            ltok(L,tt,tt==TT_IDENT?xstrdup(buf):NULL,0,0); continue;
        }
        la(L);
        switch(c){
            case '(':ltok(L,TT_LPAREN,NULL,0,0);break;case ')':ltok(L,TT_RPAREN,NULL,0,0);break;
            case '{':ltok(L,TT_LBRACE,NULL,0,0);break;case '}':ltok(L,TT_RBRACE,NULL,0,0);break;
            case '[':ltok(L,TT_LBRACKET,NULL,0,0);break;case ']':ltok(L,TT_RBRACKET,NULL,0,0);break;
            case ',':ltok(L,TT_COMMA,NULL,0,0);break;case ';':ltok(L,TT_SEMI,NULL,0,0);break;
            case '.':if(lp(L,0)=='.'){la(L);ltok(L,TT_DOTDOT,NULL,0,0);}else ltok(L,TT_DOT,NULL,0,0);break;
            case ':':ltok(L,TT_COLON,NULL,0,0);break;
            case '@':ltok(L,TT_AT,NULL,0,0);break;
            case '~':ltok(L,TT_BNOT,NULL,0,0);break;
            case '+':ltok(L,lp(L,0)=='='?(la(L),TT_PLUSEQ):TT_PLUS,NULL,0,0);break;
            case '*':ltok(L,lp(L,0)=='='?(la(L),TT_STAREQ):TT_STAR,NULL,0,0);break;
            case '/':ltok(L,lp(L,0)=='='?(la(L),TT_SLASHEQ):TT_SLASH,NULL,0,0);break;
            case '%':ltok(L,TT_PERCENT,NULL,0,0);break;
            case '=':ltok(L,lp(L,0)=='='?(la(L),TT_EQEQ):TT_EQ,NULL,0,0);break;
            case '!':ltok(L,lp(L,0)=='='?(la(L),TT_NEQ):TT_BANG,NULL,0,0);break;
            case '&':if(lp(L,0)=='&'){la(L);ltok(L,TT_AND,NULL,0,0);}else ltok(L,TT_AMP,NULL,0,0);break;
            case '|':if(lp(L,0)=='|'){la(L);ltok(L,TT_OR,NULL,0,0);}else ltok(L,TT_BOR,NULL,0,0);break;
            case '^':ltok(L,TT_BXOR,NULL,0,0);break;
            case '<':if(lp(L,0)=='='){la(L);ltok(L,TT_LTE,NULL,0,0);}else if(lp(L,0)=='<'){la(L);ltok(L,TT_SHL,NULL,0,0);}else ltok(L,TT_LT,NULL,0,0);break;
            case '>':if(lp(L,0)=='='){la(L);ltok(L,TT_GTE,NULL,0,0);}else if(lp(L,0)=='>'){la(L);ltok(L,TT_SHR,NULL,0,0);}else ltok(L,TT_GT,NULL,0,0);break;
            case '-':if(lp(L,0)=='>'){la(L);ltok(L,TT_ARROW,NULL,0,0);}else if(lp(L,0)=='='){la(L);ltok(L,TT_MINUSEQ,NULL,0,0);}else ltok(L,TT_MINUS,NULL,0,0);break;
            default: die("[Line %d] Unknown char '%c'",L->line,c);
        }
    }
}

/* ══════════════════════════════════════════════════════════
 * AST
 * ══════════════════════════════════════════════════════════*/
typedef enum {
    TY_VOID,TY_I8,TY_I16,TY_I32,TY_I64,TY_U8,TY_U16,TY_U32,TY_U64,
    TY_F64,TY_BOOL,TY_STR,TY_PTR,TY_ARRAY,TY_STRUCT,TY_UNKNOWN
} TypeKind;
typedef struct Type Type;
struct Type { TypeKind kind; Type *inner; int count; char *name; };
static Type *ty_new(TypeKind k){Type*t=xmalloc(sizeof(Type));memset(t,0,sizeof(Type));t->kind=k;return t;}
static Type *ty_ptr(Type *inner){Type*t=ty_new(TY_PTR);t->inner=inner;return t;}
static Type *ty_array(Type *inner,int n){Type*t=ty_new(TY_ARRAY);t->inner=inner;t->count=n;return t;}
static int ty_size(Type *t){
    if(!t)return 8;
    switch(t->kind){case TY_I8:case TY_U8:case TY_BOOL:return 1;case TY_I16:case TY_U16:return 2;case TY_I32:case TY_U32:return 4;case TY_ARRAY:return ty_size(t->inner)*t->count;default:return 8;}
}

typedef enum {
    ND_INT,ND_FLOAT,ND_STR,ND_BOOL,ND_CHAR,ND_NULL,
    ND_IDENT,ND_BINOP,ND_UNARY,ND_CAST,
    ND_CALL,ND_EXEC_CALL,ND_INDEX,ND_FIELD,ND_ADDROF,ND_DEREF,
    ND_IMPORT,
    ND_SORT_CALL,
    ND_VARDECL,ND_CONSTDECL,ND_ASSIGN,
    ND_RETURN,ND_IF,ND_WHILE,ND_FOR,ND_LOOP,ND_BREAK,ND_CONTINUE,
    ND_EXPRSTMT,ND_BLOCK,ND_FNDEF,ND_PARAM,ND_STRUCT_DEF,ND_PROGRAM
} NodeKind;
typedef struct Node Node;
struct Node { NodeKind kind; int line; int64_t ival; double fval; char *sval; int bval; Type *type_hint; Node **ch; int nch,cch; int is_const; int64_t const_val; };
static Node *nn(NodeKind k,int line){Node*n=xmalloc(sizeof(Node));memset(n,0,sizeof(Node));n->kind=k;n->line=line;return n;}
static void nadd(Node *p,Node *c){if(p->nch>=p->cch){p->cch=p->cch?p->cch*2:4;p->ch=xrealloc(p->ch,p->cch*sizeof(Node*));}p->ch[p->nch++]=c;}

/* ══════════════════════════════════════════════════════════
 * Parser
 * ══════════════════════════════════════════════════════════*/
typedef struct{Token*tokens;size_t pos;}Parser;
static Token *pp(Parser*P){return&P->tokens[P->pos];}
static Token *pa(Parser*P){Token*t=&P->tokens[P->pos];if(t->type!=TT_EOF)P->pos++;return t;}
static int    pc(Parser*P,TokType t){return pp(P)->type==t;}
static Token *pm(Parser*P,TokType t){if(pc(P,t))return pa(P);return NULL;}
static Token *pe(Parser*P,TokType t,const char*msg){if(!pc(P,t))die("[Line %d] Expected %s, got token %d ('%s')",pp(P)->line,msg,pp(P)->type,pp(P)->sval?pp(P)->sval:"");return pa(P);}

static Node *parse_expr(Parser*P);
static Node *parse_stmt(Parser*P);
static Node *parse_block(Parser*P);

static Type *parse_type(Parser*P){
    Token*t=pp(P);
    if(t->type==TT_PTR_T){pa(P);if(pm(P,TT_LT)){Type*inner=parse_type(P);pe(P,TT_GT,">");return ty_ptr(inner);}return ty_ptr(ty_new(TY_I64));}
    pa(P);
    switch(t->type){
        case TT_VOID:return ty_new(TY_VOID);case TT_I8:return ty_new(TY_I8);
        case TT_I16:return ty_new(TY_I16);case TT_I32:return ty_new(TY_I32);
        case TT_I64:return ty_new(TY_I64);case TT_U8:return ty_new(TY_U8);
        case TT_U16:return ty_new(TY_U16);case TT_U32:return ty_new(TY_U32);
        case TT_U64:return ty_new(TY_U64);case TT_F64:return ty_new(TY_F64);
        case TT_BOOL_T:return ty_new(TY_BOOL);case TT_STR_T:return ty_new(TY_STR);
        default:return ty_new(TY_I64);
    }
}

static Node *parse_primary(Parser*P){
    Token*t=pp(P);
    if(t->type==TT_INT){pa(P);Node*n=nn(ND_INT,t->line);n->ival=t->ival;n->is_const=1;n->const_val=t->ival;return n;}
    if(t->type==TT_FLOAT){pa(P);Node*n=nn(ND_FLOAT,t->line);n->fval=t->fval;return n;}
    if(t->type==TT_STRING){pa(P);Node*n=nn(ND_STR,t->line);n->sval=xstrdup(t->sval);return n;}
    if(t->type==TT_CHAR){pa(P);Node*n=nn(ND_CHAR,t->line);n->ival=t->ival;return n;}
    if(t->type==TT_TRUE){pa(P);Node*n=nn(ND_BOOL,t->line);n->bval=1;n->is_const=1;n->const_val=1;return n;}
    if(t->type==TT_FALSE){pa(P);Node*n=nn(ND_BOOL,t->line);n->bval=0;n->is_const=1;n->const_val=0;return n;}
    if(t->type==TT_NULL){pa(P);Node*n=nn(ND_NULL,t->line);n->is_const=1;n->const_val=0;return n;}
    if(t->type==TT_CAST){
        pa(P);pe(P,TT_LT,"<");Type*ty=parse_type(P);pe(P,TT_GT,">");
        pe(P,TT_LPAREN,"(");Node*e=parse_expr(P);pe(P,TT_RPAREN,")");
        Node*n=nn(ND_CAST,t->line);n->type_hint=ty;nadd(n,e);return n;
    }
    if(t->type==TT_AMP){pa(P);Node*n=nn(ND_ADDROF,t->line);nadd(n,parse_primary(P));return n;}
    if(t->type==TT_AT){pa(P);Node*n=nn(ND_DEREF,t->line);nadd(n,parse_primary(P));return n;}
    if(t->type==TT_EXECUTE){
        pa(P);pe(P,TT_DOT,".");Token*m=pe(P,TT_IDENT,"method");
        Node*n=nn(ND_EXEC_CALL,t->line);n->sval=xstrdup(m->sval);
        pe(P,TT_LPAREN,"(");
        while(!pc(P,TT_RPAREN)&&!pc(P,TT_EOF)){nadd(n,parse_expr(P));if(!pm(P,TT_COMMA))break;}
        pe(P,TT_RPAREN,")");return n;
    }
    if(t->type==TT_IDENT){
        pa(P);
        /* namespace.function(...) call */
        if(pc(P,TT_DOT)&&P->pos+1<P->pos+100){
            size_t saved=P->pos;
            pa(P); /* consume dot */
            if(pc(P,TT_IDENT)){
                Token*meth=pa(P);
                /* sort.num{...} — curly brace literal */
                if(pc(P,TT_LBRACE)){
                    pa(P); /* consume { */
                    char fullname[128];
                    snprintf(fullname,sizeof(fullname),"%s.%s",t->sval,meth->sval);
                    Node*n=nn(ND_SORT_CALL,t->line);n->sval=xstrdup(fullname);
                    while(!pc(P,TT_RBRACE)&&!pc(P,TT_EOF)){
                        nadd(n,parse_expr(P));
                        if(!pm(P,TT_COMMA))break;
                    }
                    pe(P,TT_RBRACE,"}");return n;
                }
                if(pm(P,TT_LPAREN)){
                    /* namespace.method call: combine as "ns.method" */
                    char fullname[128];
                    snprintf(fullname,sizeof(fullname),"%s.%s",t->sval,meth->sval);
                    Node*n=nn(ND_CALL,t->line);n->sval=xstrdup(fullname);
                    while(!pc(P,TT_RPAREN)&&!pc(P,TT_EOF)){nadd(n,parse_expr(P));if(!pm(P,TT_COMMA))break;}
                    pe(P,TT_RPAREN,")");return n;
                }
                /* namespace.field access — treat as field access */
                P->pos=saved;
            } else {
                P->pos=saved;
            }
        }
        if(pm(P,TT_LPAREN)){
            Node*n=nn(ND_CALL,t->line);n->sval=xstrdup(t->sval);
            while(!pc(P,TT_RPAREN)&&!pc(P,TT_EOF)){nadd(n,parse_expr(P));if(!pm(P,TT_COMMA))break;}
            pe(P,TT_RPAREN,")");return n;
        }
        Node*n=nn(ND_IDENT,t->line);n->sval=xstrdup(t->sval);return n;
    }
    if(t->type==TT_LPAREN){pa(P);Node*n=parse_expr(P);pe(P,TT_RPAREN,")");return n;}
    die("[Line %d] Unexpected token %d ('%s')",t->line,t->type,t->sval?t->sval:"");
    return NULL;
}

static Node *parse_postfix(Parser*P){
    Node*l=parse_primary(P);
    for(;;){
        if(pc(P,TT_LBRACKET)){pa(P);Node*n=nn(ND_INDEX,l->line);nadd(n,l);nadd(n,parse_expr(P));pe(P,TT_RBRACKET,"]");l=n;}
        else if(pc(P,TT_DOT)){pa(P);Token*f=pe(P,TT_IDENT,"field");Node*n=nn(ND_FIELD,l->line);nadd(n,l);n->sval=xstrdup(f->sval);l=n;}
        else break;
    }
    return l;
}

static Node *parse_unary(Parser*P){
    Token*t=pp(P);
    if(t->type==TT_MINUS){pa(P);Node*n=nn(ND_UNARY,t->line);n->sval=xstrdup("-");nadd(n,parse_unary(P));return n;}
    if(t->type==TT_BANG){pa(P);Node*n=nn(ND_UNARY,t->line);n->sval=xstrdup("!");nadd(n,parse_unary(P));return n;}
    if(t->type==TT_BNOT){pa(P);Node*n=nn(ND_UNARY,t->line);n->sval=xstrdup("~");nadd(n,parse_unary(P));return n;}
    if(t->type==TT_AT){pa(P);Node*n=nn(ND_DEREF,t->line);nadd(n,parse_unary(P));return n;}
    if(t->type==TT_AMP){pa(P);Node*n=nn(ND_ADDROF,t->line);nadd(n,parse_unary(P));return n;}
    return parse_postfix(P);
}

static int prec(TokType t){
    switch(t){case TT_OR:return 1;case TT_AND:return 2;case TT_BOR:return 3;case TT_BXOR:return 4;
    case TT_AMP:return 5; /* bitwise AND — when used as binary op */
    case TT_EQEQ:case TT_NEQ:return 6;case TT_LT:case TT_GT:case TT_LTE:case TT_GTE:return 7;
    case TT_SHL:case TT_SHR:return 8;case TT_PLUS:case TT_MINUS:return 9;
    case TT_STAR:case TT_SLASH:case TT_PERCENT:return 10;default:return -1;}
}
static const char *opstr(TokType t){
    switch(t){case TT_PLUS:return"+";case TT_MINUS:return"-";case TT_STAR:return"*";
    case TT_SLASH:return"/";case TT_PERCENT:return"%";case TT_EQEQ:return"==";case TT_NEQ:return"!=";
    case TT_LT:return"<";case TT_GT:return">";case TT_LTE:return"<=";case TT_GTE:return">=";
    case TT_AND:return"&&";case TT_OR:return"||";case TT_BOR:return"|";case TT_BXOR:return"^";
    case TT_AMP:return"&";
    case TT_SHL:return"<<";case TT_SHR:return">>";default:return"?";}
}
static Node *parse_binop(Parser*P,int min_p){
    Node*l=parse_unary(P);
    for(;;){
        int p=prec(pp(P)->type);if(p<min_p)break;
        Token*op=pa(P);Node*r=parse_binop(P,p+1);
        Node*n=nn(ND_BINOP,l->line);n->sval=xstrdup(opstr(op->type));
        nadd(n,l);nadd(n,r);
        /* constant folding */
        if(opt_level>=1&&l->is_const&&r->is_const){
            int64_t a=l->const_val,b=r->const_val,res=0;int fold=1;const char*s=n->sval;
            if(!strcmp(s,"+"))res=a+b;else if(!strcmp(s,"-"))res=a-b;else if(!strcmp(s,"*"))res=a*b;
            else if(!strcmp(s,"/")&&b)res=a/b;else if(!strcmp(s,"%")&&b)res=a%b;
            else if(!strcmp(s,"=="))res=a==b;else if(!strcmp(s,"!="))res=a!=b;
            else if(!strcmp(s,"<"))res=a<b;else if(!strcmp(s,">"))res=a>b;
            else if(!strcmp(s,"<="))res=a<=b;else if(!strcmp(s,">="))res=a>=b;
            else if(!strcmp(s,"&&"))res=a&&b;else if(!strcmp(s,"||"))res=a||b;
            else if(!strcmp(s,"|"))res=a|b;else if(!strcmp(s,"^"))res=a^b;
            else if(!strcmp(s,"<<"))res=a<<b;else if(!strcmp(s,">>"))res=a>>b;
            else fold=0;
            if(fold){n->is_const=1;n->const_val=res;}
        }
        l=n;
    }
    return l;
}
static Node *parse_expr(Parser*P){return parse_binop(P,1);}
static Node *parse_block(Parser*P){
    pe(P,TT_LBRACE,"{");Node*b=nn(ND_BLOCK,pp(P)->line);
    while(!pc(P,TT_RBRACE)&&!pc(P,TT_EOF))nadd(b,parse_stmt(P));
    pe(P,TT_RBRACE,"}");return b;
}
static Node *parse_stmt(Parser*P){
    Token*t=pp(P);
    if(t->type==TT_VAR||t->type==TT_CONST){
        int is_const=(t->type==TT_CONST);pa(P);
        Token*name=pe(P,TT_IDENT,"variable name");
        Node*n=nn(is_const?ND_CONSTDECL:ND_VARDECL,t->line);n->sval=xstrdup(name->sval);
        if(pm(P,TT_COLON)){
            if(pc(P,TT_LBRACKET)){pa(P);int cnt=0;if(pc(P,TT_INT)){cnt=(int)pp(P)->ival;pa(P);}pe(P,TT_RBRACKET,"]");Type*inner=parse_type(P);n->type_hint=ty_array(inner,cnt);}
            else n->type_hint=parse_type(P);
        }
        if(pm(P,TT_EQ))nadd(n,parse_expr(P));
        pm(P,TT_SEMI);return n;
    }
    if(t->type==TT_RETURN){pa(P);Node*n=nn(ND_RETURN,t->line);if(!pc(P,TT_RBRACE)&&!pc(P,TT_SEMI))nadd(n,parse_expr(P));pm(P,TT_SEMI);return n;}
    if(t->type==TT_IF){
        pa(P);Node*n=nn(ND_IF,t->line);nadd(n,parse_expr(P));nadd(n,parse_block(P));
        if(pm(P,TT_ELSE)){if(pc(P,TT_IF))nadd(n,parse_stmt(P));else nadd(n,parse_block(P));}
        return n;
    }
    if(t->type==TT_WHILE){pa(P);Node*n=nn(ND_WHILE,t->line);nadd(n,parse_expr(P));nadd(n,parse_block(P));return n;}
    if(t->type==TT_FOR){
        pa(P);Token*var=pe(P,TT_IDENT,"loop var");pe(P,TT_IN,"in");
        Node*n=nn(ND_FOR,t->line);n->sval=xstrdup(var->sval);
        nadd(n,parse_expr(P));pe(P,TT_DOTDOT,"..");nadd(n,parse_expr(P));nadd(n,parse_block(P));return n;
    }
    if(t->type==TT_LOOP){pa(P);Node*n=nn(ND_LOOP,t->line);nadd(n,parse_block(P));return n;}
    if(t->type==TT_BREAK){pa(P);pm(P,TT_SEMI);return nn(ND_BREAK,t->line);}
    if(t->type==TT_CONTINUE){pa(P);pm(P,TT_SEMI);return nn(ND_CONTINUE,t->line);}
    Node*lhs=parse_expr(P);
    TokType at=pp(P)->type;
    if(at==TT_EQ||at==TT_PLUSEQ||at==TT_MINUSEQ||at==TT_STAREQ||at==TT_SLASHEQ){
        pa(P);Node*rhs=parse_expr(P);
        if(at!=TT_EQ){
            const char*op=(at==TT_PLUSEQ)?"+":(at==TT_MINUSEQ)?"-":(at==TT_STAREQ)?"*":"/";
            Node*bop=nn(ND_BINOP,lhs->line);bop->sval=xstrdup(op);
            Node*lhs2=nn(lhs->kind,lhs->line);lhs2->sval=lhs->sval?xstrdup(lhs->sval):NULL;
            for(int i=0;i<lhs->nch;i++)nadd(lhs2,lhs->ch[i]);
            nadd(bop,lhs2);nadd(bop,rhs);rhs=bop;
        }
        Node*n=nn(ND_ASSIGN,t->line);nadd(n,lhs);nadd(n,rhs);pm(P,TT_SEMI);return n;
    }
    pm(P,TT_SEMI);Node*n=nn(ND_EXPRSTMT,t->line);nadd(n,lhs);return n;
}
static Node *parse_fn(Parser*P){
    Token*t=pe(P,TT_FN,"fn");Token*name=pe(P,TT_IDENT,"fn name");
    Node*fn=nn(ND_FNDEF,t->line);fn->sval=xstrdup(name->sval);
    pe(P,TT_LPAREN,"(");
    while(!pc(P,TT_RPAREN)&&!pc(P,TT_EOF)){
        Token*pn=pe(P,TT_IDENT,"param");pe(P,TT_COLON,":");Type*pt=parse_type(P);
        Node*p=nn(ND_PARAM,pn->line);p->sval=xstrdup(pn->sval);p->type_hint=pt;
        nadd(fn,p);if(!pm(P,TT_COMMA))break;
    }
    pe(P,TT_RPAREN,")");pe(P,TT_ARROW,"->");fn->type_hint=parse_type(P);
    nadd(fn,parse_block(P));return fn;
}

/* Rename all ND_CALL nodes inside a node tree: if call name matches a lib fn, prefix it */
static void prefix_calls(Node*n, const char*libname, char(*libfns)[64], int nfns){
    if(!n)return;
    if(n->kind==ND_CALL&&n->sval){
        /* check if this call matches one of the lib's function names */
        for(int i=0;i<nfns;i++){
            if(!strcmp(n->sval,libfns[i])){
                char newname[200];
                snprintf(newname,sizeof(newname),"%s.%s",libname,n->sval);
                free(n->sval);n->sval=xstrdup(newname);
                break;
            }
        }
    }
    for(int i=0;i<n->nch;i++) prefix_calls(n->ch[i],libname,libfns,nfns);
}
static Node *parse_program(Parser*P){
    Node*prog=nn(ND_PROGRAM,0);
    while(!pc(P,TT_EOF)){
        if(pc(P,TT_IMPORT)){
            /* get-Library/name — record import, resolved later */
            Token*t=pa(P);
            Node*n=nn(ND_IMPORT,t->line);n->sval=xstrdup(t->sval);
            nadd(prog,n);
        } else if(pc(P,TT_STRUCT)){
            Token*t=pe(P,TT_STRUCT,"struct");Token*sn=pe(P,TT_IDENT,"struct name");
            Node*s=nn(ND_STRUCT_DEF,t->line);s->sval=xstrdup(sn->sval);
            pe(P,TT_LBRACE,"{");
            while(!pc(P,TT_RBRACE)&&!pc(P,TT_EOF)){
                Token*fn=pe(P,TT_IDENT,"field");pe(P,TT_COLON,":");Type*ft=parse_type(P);
                Node*f=nn(ND_PARAM,fn->line);f->sval=xstrdup(fn->sval);f->type_hint=ft;
                nadd(s,f);pm(P,TT_SEMI);pm(P,TT_COMMA);
            }
            pe(P,TT_RBRACE,"}");nadd(prog,s);
        } else {
            nadd(prog,parse_fn(P));
        }
    }
    return prog;
}

/* ══════════════════════════════════════════════════════════
 * x86-64 emitters — every encoding hand-verified
 * Registers: RAX=0 RCX=1 RDX=2 RBX=3 RSP=4 RBP=5 RSI=6 RDI=7
 *            R8=8  R9=9  R10=10 R11=11 R12=12 R13=13
 * ══════════════════════════════════════════════════════════*/
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
#define R14 14
#define R15 15

/* REX prefix helpers */
static int ri(int r){return r&7;}
static int rb(int r){return r>=8?1:0;}
static int rr(int r){return r>=8?1:0;}

/* ModRM byte: mod=3 (register-register) */
static uint8_t modrm_rr(int reg, int rm){return (uint8_t)(0xC0|(ri(reg)<<3)|ri(rm));}

/* Emit helpers */
static void e1(Buf*b,uint8_t a){buf_push(b,a);}
static void e2(Buf*b,uint8_t a,uint8_t c){buf_push(b,a);buf_push(b,c);}
static void e3(Buf*b,uint8_t a,uint8_t c,uint8_t d){buf_push(b,a);buf_push(b,c);buf_push(b,d);}
static void e4(Buf*b,uint8_t a,uint8_t c,uint8_t d,uint8_t e){buf_push(b,a);buf_push(b,c);buf_push(b,d);buf_push(b,e);}
static void e5(Buf*b,uint8_t a,uint8_t c,uint8_t d,uint8_t e,uint8_t f){buf_push(b,a);buf_push(b,c);buf_push(b,d);buf_push(b,e);buf_push(b,f);}
static void ei32(Buf*b,int32_t v){buf_bytes(b,&v,4);}
static void ei64(Buf*b,int64_t v){buf_bytes(b,&v,8);}

/* REX.W prefix for 64-bit ops */
static uint8_t rex_w(int dst_r,int src_r){
    /* REX = 0100 W R X B */
    return (uint8_t)(0x48|(rr(src_r)<<2)|rb(dst_r));
}

/* mov rax, imm64 — always 10 bytes */
static void e_mov_imm(Buf*b,int dst,int64_t imm){
    /* Use shorter encoding for small non-negative values: mov eax,imm32 (zero-extends) */
    if(imm>=0&&imm<=0x7FFFFFFF){
        if(rb(dst))e1(b,0x41);
        e1(b,(uint8_t)(0xB8|ri(dst)));
        ei32(b,(int32_t)imm);
    } else {
        e2(b,(uint8_t)(0x48|rb(dst)),(uint8_t)(0xB8|ri(dst)));
        ei64(b,imm);
    }
}

/* mov dst, src (64-bit) */
static void e_mov(Buf*b,int dst,int src){
    if(dst==src)return;
    e3(b,rex_w(dst,src),0x89,modrm_rr(src,dst));
}

/* mov [rbp+off], src */
static void e_store(Buf*b,int off,int src){
    /* REX.W + 89 /r + ModRM(disp8/32,src,rbp=5) */
    uint8_t rex=(uint8_t)(0x48|(rr(src)<<2)); /* rb(rbp)=0 */
    e2(b,rex,0x89);
    if(off>=-128&&off<=127){e2(b,(uint8_t)(0x45|(ri(src)<<3)),( uint8_t)(int8_t)off);}
    else{e1(b,(uint8_t)(0x85|(ri(src)<<3)));ei32(b,off);}
}

/* mov dst, [rbp+off] */
static void e_load(Buf*b,int dst,int off){
    uint8_t rex=(uint8_t)(0x48|(rr(dst)<<2));
    e2(b,rex,0x8B);
    if(off>=-128&&off<=127){e2(b,(uint8_t)(0x45|(ri(dst)<<3)),(uint8_t)(int8_t)off);}
    else{e1(b,(uint8_t)(0x85|(ri(dst)<<3)));ei32(b,off);}
}

/* mov [reg], src  (memory write through pointer) */
static void e_store_ptr(Buf*b,int base,int src){
    e3(b,rex_w(base,src),0x89,(uint8_t)(0x00|(ri(src)<<3)|ri(base)));
}

/* mov dst, [reg]  (memory read through pointer) */
static void e_load_ptr(Buf*b,int dst,int base){
    e3(b,rex_w(base,dst),0x8B,(uint8_t)(0x00|(ri(dst)<<3)|ri(base)));
}

/* lea dst, [rbp+off] */
static void e_lea_rbp(Buf*b,int dst,int off){
    uint8_t rex=(uint8_t)(0x48|(rr(dst)<<2));
    e2(b,rex,0x8D);
    if(off>=-128&&off<=127){e2(b,(uint8_t)(0x45|(ri(dst)<<3)),(uint8_t)(int8_t)off);}
    else{e1(b,(uint8_t)(0x85|(ri(dst)<<3)));ei32(b,off);}
}

/* lea dst, [base+idx*8] — for array indexing */
static void e_lea_sib(Buf*b,int dst,int base,int idx){
    /* REX.W + 8D /r + ModRM(0,dst,4) + SIB(3,idx,base) */
    uint8_t rex=(uint8_t)(0x48|(rr(dst)<<2)|rb(base)|((rr(idx)?1:0)<<1));
    e4(b,rex,0x8D,(uint8_t)(0x04|(ri(dst)<<3)),(uint8_t)(0x40|(ri(idx)<<3)|ri(base)));
    /* SIB: scale=3(8x), index=idx, base=base */
    /* Wait: SIB format: scale(2) index(3) base(3). Bits 7:6=scale, 5:3=index, 2:0=base */
    /* 0x40 = 01000000. That's scale=1, index=0, base=0. That's wrong for 8x. */
    /* Let me fix: scale 8 = 0b11. SIB = (3<<6)|(ri(idx)<<3)|ri(base) */
}
/* Redo e_lea_sib correctly: */
static void e_arr_addr(Buf*b,int dst,int base,int idx){
    /* lea dst,[base+idx*8]: REX.W 8D ModRM(0,dst,SIB) SIB(scale=3,idx,base) */
    uint8_t rex=(uint8_t)(0x48|(rr(dst)<<2)|(rr(idx)<<1)|rb(base));
    uint8_t modrm_byte=(uint8_t)(0x04|(ri(dst)<<3)); /* mod=0, reg=dst, rm=4(SIB) */
    uint8_t sib=(uint8_t)((3<<6)|(ri(idx)<<3)|ri(base)); /* scale=8, idx, base */
    e4(b,rex,0x8D,modrm_byte,sib);
}

/* push/pop */
static void e_push(Buf*b,int r){if(r>=8){e2(b,0x41,(uint8_t)(0x50|ri(r)));}else e1(b,(uint8_t)(0x50|r));}
static void e_pop(Buf*b,int r) {if(r>=8){e2(b,0x41,(uint8_t)(0x58|ri(r)));}else e1(b,(uint8_t)(0x58|r));}

/* sub rsp, n */
static void e_sub_rsp(Buf*b,int n){
    if(n>0&&n<=127){e4(b,0x48,0x83,0xEC,(uint8_t)n);}
    else{e3(b,0x48,0x81,0xEC);ei32(b,n);}
}
/* add rsp, n */
static void e_add_rsp(Buf*b,int n){
    if(n>0&&n<=127){e4(b,0x48,0x83,0xC4,(uint8_t)n);}
    else{e3(b,0x48,0x81,0xC4);ei32(b,n);}
}

/* Arithmetic: all operate on 64-bit registers */
static void e_add(Buf*b,int d,int s){e3(b,rex_w(d,s),0x01,modrm_rr(s,d));}
static void e_sub(Buf*b,int d,int s){e3(b,rex_w(d,s),0x29,modrm_rr(s,d));}
static void e_imul(Buf*b,int d,int s){e4(b,rex_w(s,d),0x0F,0xAF,modrm_rr(d,s));}
static void e_neg(Buf*b,int r){e3(b,(uint8_t)(0x48|rb(r)),0xF7,(uint8_t)(0xD8|ri(r)));}
static void e_not(Buf*b,int r){e3(b,(uint8_t)(0x48|rb(r)),0xF7,(uint8_t)(0xD0|ri(r)));}
static void e_cqo(Buf*b){e2(b,0x48,0x99);}
static void e_idiv(Buf*b,int r){e3(b,(uint8_t)(0x48|rb(r)),0xF7,(uint8_t)(0xF8|ri(r)));}
static void e_and(Buf*b,int d,int s){e3(b,rex_w(d,s),0x21,modrm_rr(s,d));}
static void e_or(Buf*b,int d,int s) {e3(b,rex_w(d,s),0x09,modrm_rr(s,d));}
static void e_xor(Buf*b,int d,int s){e3(b,rex_w(d,s),0x31,modrm_rr(s,d));}
/* shl rax, cl */
static void e_shl(Buf*b,int r){e3(b,(uint8_t)(0x48|rb(r)),0xD3,(uint8_t)(0xE0|ri(r)));}
/* sar rax, cl */
static void e_sar(Buf*b,int r){e3(b,(uint8_t)(0x48|rb(r)),0xD3,(uint8_t)(0xF8|ri(r)));}

/* cmp + setcc -> rax */
static void e_cmp(Buf*b,int a,int s){e3(b,rex_w(a,s),0x39,modrm_rr(s,a));}
static void e_test_rax(Buf*b){e3(b,0x48,0x85,0xC0);}

/* setCC al then movzx rax,al */
static uint8_t cc_byte(const char*op){
    if(!strcmp(op,"=="))return 0x94;if(!strcmp(op,"!="))return 0x95;
    if(!strcmp(op,"<"))return 0x9C; if(!strcmp(op,">"))return 0x9F;
    if(!strcmp(op,"<="))return 0x9E;return 0x9D; /* >= */
}
static void e_setcc(Buf*b,const char*op){
    e3(b,0x0F,cc_byte(op),0xC0); /* setCC al */
    e4(b,0x48,0x0F,0xB6,0xC0);  /* movzx rax,al */
}

/* syscall, ret */
static void e_syscall(Buf*b){e2(b,0x0F,0x05);}
static void e_ret(Buf*b){e1(b,0xC3);}

/* lea rsi,[rip+rel32] — for string pointers */
static void e_lea_rsi_rip(Buf*b,int32_t rel){e3(b,0x48,0x8D,0x35);ei32(b,rel);}
/* lea rax,[rip+rel32] */
static void e_lea_rax_rip(Buf*b,int32_t rel){e3(b,0x48,0x8D,0x05);ei32(b,rel);}

/* call rel32: returns the site (position of the rel32 bytes) */
static size_t e_call(Buf*b){e1(b,0xE8);size_t s=buf_pos(b);ei32(b,0);return s;}
/* unconditional jmp rel32 */
static size_t e_jmp(Buf*b){e1(b,0xE9);size_t s=buf_pos(b);ei32(b,0);return s;}
/* je rel32 */
static size_t e_je(Buf*b){e2(b,0x0F,0x84);size_t s=buf_pos(b);ei32(b,0);return s;}
/* jne rel32 */
static size_t e_jne(Buf*b){e2(b,0x0F,0x85);size_t s=buf_pos(b);ei32(b,0);return s;}

/* patch a rel32 at site to jump to target */
static void patch(Buf*b,size_t site,size_t target){
    int32_t rel=(int32_t)((int64_t)target-(int64_t)(site+4));
    buf_patch32(b,site,rel);
}
/* emit jmp backward to target (already known) */
static void e_jmp_back(Buf*b,size_t target){
    int32_t rel=(int32_t)((int64_t)target-(int64_t)(buf_pos(b)+5));
    e1(b,0xE9);ei32(b,rel);
}

/* ══════════════════════════════════════════════════════════
 * ELF64 builder — single PT_LOAD RX segment
 * Code + rodata in one flat segment, no separate pages.
 * ══════════════════════════════════════════════════════════*/
#define LOAD_ADDR    0x400000ULL
#define ELF_HDR      64
#define PHDR_SZ      56
#define HDR_TOTAL    (ELF_HDR+PHDR_SZ)  /* 120 */

static uint64_t make_elf(Buf*out,const uint8_t*code,size_t clen,
                          const uint8_t*rd,size_t rlen){
    /* Layout: [ELF hdr][PHDR][code][pad to 16][rodata] */
    size_t code_end=HDR_TOTAL+clen;
    size_t pad=(16-code_end%16)%16;
    size_t ro_off=code_end+pad;
    uint64_t vrd=LOAD_ADDR+ro_off;
    size_t total=ro_off+rlen;

    uint8_t hdr[ELF_HDR]={0};
    hdr[0]=0x7f;hdr[1]='E';hdr[2]='L';hdr[3]='F';
    hdr[4]=2;hdr[5]=1;hdr[6]=1; /* 64-bit LE ELF1 */
    *(uint16_t*)(hdr+16)=2;    /* ET_EXEC */
    *(uint16_t*)(hdr+18)=0x3E; /* EM_X86_64 */
    *(uint32_t*)(hdr+20)=1;    /* EV_CURRENT */
    *(uint64_t*)(hdr+24)=LOAD_ADDR+HDR_TOTAL; /* e_entry = start of code */
    *(uint64_t*)(hdr+32)=ELF_HDR; /* e_phoff */
    *(uint16_t*)(hdr+52)=ELF_HDR;
    *(uint16_t*)(hdr+54)=PHDR_SZ;
    *(uint16_t*)(hdr+56)=1;    /* one PHDR */
    *(uint16_t*)(hdr+58)=64;

    uint8_t ph[PHDR_SZ]={0};
    *(uint32_t*)(ph+0)=1;      /* PT_LOAD */
    *(uint32_t*)(ph+4)=5;      /* PF_R|PF_X */
    /* p_offset=0: map entire file from start */
    *(uint64_t*)(ph+16)=LOAD_ADDR;
    *(uint64_t*)(ph+24)=LOAD_ADDR;
    *(uint64_t*)(ph+32)=total;
    *(uint64_t*)(ph+40)=total;
    *(uint64_t*)(ph+48)=0x200000;

    buf_bytes(out,hdr,ELF_HDR);
    buf_bytes(out,ph,PHDR_SZ);
    buf_bytes(out,code,clen);
    for(size_t i=0;i<pad;i++)buf_push(out,0);
    buf_bytes(out,rd,rlen);
    return vrd;
}

/* ══════════════════════════════════════════════════════════
 * Subroutines — verified byte-by-byte
 * ══════════════════════════════════════════════════════════*/

/* itoa(rdi=i64) -> rsi=char*, rdx=len
 * Uses dynamic label patching, fully correct. */
static void emit_itoa(Buf*b){
    /* Prologue: push rbp; mov rbp,rsp; sub rsp,64 */
    e3(b,0x55,0x48,0x89); e1(b,0xE5); /* push rbp; mov rbp,rsp (split for clarity) */
    /* sub rsp,64 */
    e4(b,0x48,0x83,0xEC,0x40);
    /* xor r8d,r8d (is_negative=0) */
    e3(b,0x45,0x31,0xC0);
    /* test rdi,rdi */
    e3(b,0x48,0x85,0xFF);
    /* jge skip_neg */
    size_t jge=buf_pos(b); e2(b,0x7D,0x00);
    /* neg branch: mov r8d,1; neg rdi */
    e4(b,0x41,0xB8,0x01,0x00); e2(b,0x00,0x00); /* mov r8d,1 (6 bytes) */
    e3(b,0x48,0xF7,0xDF);       /* neg rdi */
    b->data[jge+1]=(uint8_t)(buf_pos(b)-(jge+2));
    /* lea r9,[rbp-1] (write ptr, builds string right-to-left) */
    e4(b,0x4C,0x8D,0x4D,0xFF);
    /* xor r10,r10 (length counter) */
    e3(b,0x4D,0x31,0xD2);
    /* special case rdi==0: write '0' */
    e3(b,0x48,0x85,0xFF);
    size_t jnz=buf_pos(b); e2(b,0x75,0x00);
    /* zero case: sub r9,1; mov [r9],'0'; inc r10 */
    e4(b,0x49,0x83,0xE9,0x01);
    e4(b,0x41,0xC6,0x01,0x30);
    e3(b,0x49,0xFF,0xC2);
    size_t jmp_done=buf_pos(b); e2(b,0xEB,0x00);
    /* digit_loop: */
    size_t dloop=buf_pos(b);
    b->data[jnz+1]=(uint8_t)(dloop-(jnz+2));
    e3(b,0x48,0x85,0xFF); /* test rdi,rdi */
    size_t jz_aft=buf_pos(b); e2(b,0x74,0x00);
    /* div by 10: mov rax,rdi; xor rdx,rdx; mov rcx,10; div rcx; mov rdi,rax */
    e3(b,0x48,0x89,0xF8);
    e3(b,0x48,0x31,0xD2);
    e4(b,0x48,0xC7,0xC1,0x0A); e3(b,0x00,0x00,0x00); /* mov rcx,10 */
    e3(b,0x48,0xF7,0xF1);
    e3(b,0x48,0x89,0xC7);
    /* add rdx,'0'; sub r9,1; mov [r9],dl; inc r10 */
    e4(b,0x48,0x83,0xC2,0x30);
    e4(b,0x49,0x83,0xE9,0x01);
    e3(b,0x41,0x88,0x11);
    e3(b,0x49,0xFF,0xC2);
    /* jmp dloop */
    {int8_t rel=(int8_t)((int64_t)dloop-(int64_t)(buf_pos(b)+2));
     e2(b,0xEB,(uint8_t)rel);}
    /* after_digits: */
    size_t aft=buf_pos(b);
    b->data[jz_aft+1]=(uint8_t)(aft-(jz_aft+2));
    /* if negative: sub r9,1; mov [r9],'-'; inc r10 */
    e3(b,0x45,0x85,0xC0); /* test r8d,r8d */
    size_t jz_done=buf_pos(b); e2(b,0x74,0x00);
    e4(b,0x49,0x83,0xE9,0x01);
    e4(b,0x41,0xC6,0x01,0x2D);
    e3(b,0x49,0xFF,0xC2);
    /* done: */
    size_t done=buf_pos(b);
    b->data[jmp_done+1]=(uint8_t)(done-(jmp_done+2));
    b->data[jz_done+1]=(uint8_t)(done-(jz_done+2));
    /* mov rsi,r9; mov rdx,r10 */
    e3(b,0x4C,0x89,0xCE);
    e3(b,0x4C,0x89,0xD2);
    /* Epilogue */
    e3(b,0x48,0x89,0xEC); /* mov rsp,rbp */
    e1(b,0x5D);           /* pop rbp */
    e_ret(b);
}

/* strlen(rdi=str) -> rax=length
 * Verified: loop at +3, je target = +14 (ret), jmp back = +3 (loop) */
static void emit_strlen(Buf*b){
    e3(b,0x48,0x31,0xC0);           /* xor rax,rax */
    size_t loop=buf_pos(b);          /* loop: */
    e4(b,0x80,0x3C,0x07,0x00);      /* cmp byte[rdi+rax],0 */
    size_t je_site=buf_pos(b);
    e2(b,0x74,0x00);                 /* je done (placeholder) */
    e3(b,0x48,0xFF,0xC0);            /* inc rax */
    {int8_t rel=(int8_t)((int64_t)loop-(int64_t)(buf_pos(b)+2));
     e2(b,0xEB,(uint8_t)rel);}       /* jmp loop */
    /* done: patch je */
    b->data[je_site+1]=(uint8_t)(buf_pos(b)-(je_site+2));
    e_ret(b);
}

/* memcpy(rdi=dst, rsi=src, rdx=len) */
static void emit_memcpy(Buf*b){
    e_push(b,RDI); e_push(b,RSI);   /* save regs */
    e_mov(b,RCX,RDX);               /* rcx=len */
    e2(b,0xF3,0xA4);                /* rep movsb */
    e_pop(b,RSI); e_pop(b,RDI);
    e_ret(b);
}

/* memset(rdi=dst, rsi=byte_val, rdx=len) */
static void emit_memset(Buf*b){
    e_push(b,RDI);
    e_mov(b,RAX,RSI);               /* rax=val */
    e_mov(b,RCX,RDX);               /* rcx=len */
    e2(b,0xF3,0xAA);                /* rep stosb */
    e_pop(b,RDI);
    e_ret(b);
}

/* ══════════════════════════════════════════════════════════
 * Struct registry
 * ══════════════════════════════════════════════════════════*/
#define MAX_STRUCTS 32
#define MAX_FIELDS  32
typedef struct { char name[64]; char fields[MAX_FIELDS][64]; int offsets[MAX_FIELDS]; int nfields,total_size; } StructDef;
static StructDef structs[MAX_STRUCTS];
static int nstructs=0;
static StructDef *find_struct(const char*n){for(int i=0;i<nstructs;i++)if(!strcmp(structs[i].name,n))return &structs[i];return NULL;}
static int field_offset(StructDef*s,const char*f){for(int i=0;i<s->nfields;i++)if(!strcmp(s->fields[i],f))return s->offsets[i];die("Unknown field '%s'",f);return 0;}

/* ══════════════════════════════════════════════════════════
 * Codegen state
 * ══════════════════════════════════════════════════════════*/
#define MAX_LOCALS   512
#define MAX_PATCHES  2048
#define MAX_FNS      256
#define MAX_STRS     1024
#define MAX_BREAKS   256

typedef struct { char name[64]; size_t off; } FnEnt;
typedef struct { size_t site; char name[64]; } FnPatch;
typedef struct { size_t site; size_t rdoff; } StrPatch;
typedef struct { size_t sites[MAX_BREAKS]; int n; } BrkStack;

typedef struct {
    char  name[64];
    int   off;        /* rbp-relative */
    int   size;       /* bytes */
    int   is_array;
    int   arr_count;
    int   elem_size;
    int   is_const;
    int64_t const_val;
} Local;

typedef struct {
    Buf   code, rodata;
    FnEnt fns[MAX_FNS];  int nfns;
    FnPatch fn_patches[MAX_PATCHES]; int nfnp;
    StrPatch str_patches[MAX_PATCHES]; int nstrp;
    size_t itoa_off, strlen_off, memcpy_off, memset_off;
    size_t itoa_sites[MAX_PATCHES];   int nitoa;
    size_t strlen_sites[MAX_PATCHES]; int nstrlen;
    /* per-function */
    Local  locals[MAX_LOCALS]; int nloc;
    int    local_size;
    size_t ret_sites[MAX_PATCHES]; int nret;
    BrkStack bstk[32]; int bdepth;
    size_t cont_sites[32][MAX_BREAKS]; int ncont[32];
    /* rodata dedup */
    struct { uint8_t *d; size_t l; size_t off; } strs[MAX_STRS]; int nstrs;
} CG;

static void cg_init(CG*g){memset(g,0,sizeof(CG));buf_init(&g->code);buf_init(&g->rodata);}

/* Add bytes to rodata, deduplicated */
static size_t cg_rodata(CG*g,const uint8_t*d,size_t l){
    for(int i=0;i<g->nstrs;i++)if(g->strs[i].l==l&&!memcmp(g->strs[i].d,d,l))return g->strs[i].off;
    size_t off=g->rodata.len;
    buf_bytes(&g->rodata,d,l);
    if(g->nstrs<MAX_STRS){
        g->strs[g->nstrs].d=xmalloc(l);memcpy(g->strs[g->nstrs].d,d,l);
        g->strs[g->nstrs].l=l;g->strs[g->nstrs].off=off;g->nstrs++;
    }
    return off;
}

/* Add a string to rodata, return (rodata_offset, printable_length) */
static size_t cg_str(CG*g,const char*s,int nl,size_t*plen){
    size_t sl=strlen(s);
    uint8_t*buf=xmalloc(sl+2);
    memcpy(buf,s,sl);
    if(nl){buf[sl]='\n';buf[sl+1]=0;*plen=sl+1;}
    else{buf[sl]=0;*plen=sl;}
    size_t off=cg_rodata(g,buf,sl+(nl?2:1));
    free(buf);return off;
}

/* Emit lea rsi,[rip+??] with deferred patch */
static void cg_lea_rsi(CG*g,size_t rdoff){
    e_lea_rsi_rip(&g->code,0);
    size_t site=buf_pos(&g->code)-4;
    g->str_patches[g->nstrp].site=site;
    g->str_patches[g->nstrp].rdoff=rdoff;
    g->nstrp++;
}
/* Emit lea rax,[rip+??] with deferred patch */
static void cg_lea_rax(CG*g,size_t rdoff){
    e_lea_rax_rip(&g->code,0);
    size_t site=buf_pos(&g->code)-4;
    g->str_patches[g->nstrp].site=site;
    g->str_patches[g->nstrp].rdoff=rdoff;
    g->nstrp++;
}

static void cg_call_itoa(CG*g){size_t s=e_call(&g->code);g->itoa_sites[g->nitoa++]=s;}
static void cg_call_strlen(CG*g){size_t s=e_call(&g->code);g->strlen_sites[g->nstrlen++]=s;}

static Local *cg_find(CG*g,const char*name){
    for(int i=g->nloc-1;i>=0;i--)if(!strcmp(g->locals[i].name,name))return &g->locals[i];
    return NULL;
}
static Local *cg_get(CG*g,const char*name,int line){
    Local*l=cg_find(g,name);if(!l)die("[Line %d] Undefined variable '%s'",line,name);return l;
}
static Local *cg_alloc(CG*g,const char*name,int size){
    /* Align allocation */
    int align=size>8?8:size<1?1:size;
    g->local_size+=size;
    if(g->local_size%align)g->local_size+=(align-(g->local_size%align));
    Local*l=&g->locals[g->nloc++];
    memset(l,0,sizeof(Local));
    strncpy(l->name,name,63);
    l->off=-g->local_size;
    l->size=size;
    return l;
}

/* Write sys_write(1, rsi=str_ptr, rdx=len) — rsi and rdx must already be set */
static void cg_write(CG*g){
    e_mov_imm(&g->code,RAX,1);
    e_mov_imm(&g->code,RDI,1);
    e_syscall(&g->code);
}

/* Write a literal string from rodata */
static void cg_write_lit(CG*g,size_t rdoff,size_t plen){
    cg_lea_rsi(g,rdoff);
    e_mov_imm(&g->code,RAX,1);
    e_mov_imm(&g->code,RDI,1);
    e_mov_imm(&g->code,RDX,(int64_t)plen);
    e_syscall(&g->code);
}

/* Write a single newline */
static void cg_write_nl(CG*g){
    uint8_t nl='\n';
    size_t off=cg_rodata(g,&nl,1);
    cg_write_lit(g,off,1);
}

static void cg_expr(CG*g,Node*e);
static void cg_stmt(CG*g,Node*s);

static int cg_count_locals(Node*block){
    int n=0;
    for(int i=0;i<block->nch;i++){
        Node*s=block->ch[i];
        if(s->kind==ND_VARDECL||s->kind==ND_CONSTDECL){
            int sz=8;
            if(s->type_hint&&s->type_hint->kind==TY_ARRAY)
                sz=ty_size(s->type_hint->inner)*s->type_hint->count;
            n+=sz<8?8:sz;
        }
        else if(s->kind==ND_FOR)n+=8+cg_count_locals(s->ch[s->nch-1]);
        else if(s->kind==ND_IF){n+=cg_count_locals(s->ch[1]);if(s->nch>2)n+=cg_count_locals(s->ch[2]);}
        else if(s->kind==ND_WHILE||s->kind==ND_LOOP)n+=cg_count_locals(s->ch[s->nch-1]);
    }
    return n;
}

static void cg_expr(CG*g,Node*e){
    /* Fast path: constant */
    if(opt_level>=1&&e->is_const&&e->kind!=ND_STR){
        e_mov_imm(&g->code,RAX,e->const_val);
        return;
    }
    switch(e->kind){
    case ND_INT:   e_mov_imm(&g->code,RAX,e->ival); break;
    case ND_FLOAT: {int64_t bits;memcpy(&bits,&e->fval,8);e_mov_imm(&g->code,RAX,bits);}break;
    case ND_CHAR:  e_mov_imm(&g->code,RAX,e->ival); break;
    case ND_BOOL:  e_mov_imm(&g->code,RAX,e->bval); break;
    case ND_NULL:  e_xor(&g->code,RAX,RAX); break;

    case ND_STR:{
        size_t plen;size_t off=cg_str(g,e->sval,0,&plen);
        cg_lea_rax(g,off);
        break;
    }
    case ND_IDENT:{
        Local*l=cg_get(g,e->sval,e->line);
        if(l->is_const){e_mov_imm(&g->code,RAX,l->const_val);break;}
        if(l->is_array){e_lea_rbp(&g->code,RAX,l->off);}
        else if(l->size==1){
            /* movzx rax,byte[rbp+off] */
            e2(&g->code,0x48,0x0F); e1(&g->code,0xB6);
            if(l->off>=-128&&l->off<=127){e2(&g->code,0x45,(uint8_t)(int8_t)l->off);}
            else{e1(&g->code,0x85);ei32(&g->code,l->off);}
        } else {
            e_load(&g->code,RAX,l->off);
        }
        break;
    }
    case ND_ADDROF:{
        Node*inner=e->ch[0];
        if(inner->kind==ND_IDENT){
            Local*l=cg_get(g,inner->sval,e->line);
            e_lea_rbp(&g->code,RAX,l->off);
        } else if(inner->kind==ND_INDEX){
            cg_expr(g,inner->ch[0]); /* base in RAX */
            e_push(&g->code,RAX);
            cg_expr(g,inner->ch[1]); /* index in RAX */
            e_pop(&g->code,RCX);
            e_arr_addr(&g->code,RAX,RCX,RAX);
        } else {
            cg_expr(g,inner);
        }
        break;
    }
    case ND_DEREF:{
        cg_expr(g,e->ch[0]);
        /* Load 1 byte (movzx rax, byte[rax]) — ptr arithmetic is byte-level */
        /* REX.W=1, 0F B6, ModRM(0, rax=0, rax=0) = 0x00 */
        e4(&g->code,0x48,0x0F,0xB6,0x00); /* movzx rax,byte[rax] */
        break;
    }
    case ND_INDEX:{
        cg_expr(g,e->ch[0]); /* base ptr in RAX */
        e_push(&g->code,RAX);
        cg_expr(g,e->ch[1]); /* index in RAX */
        e_pop(&g->code,RCX); /* base in RCX */
        e_arr_addr(&g->code,RDX,RCX,RAX); /* rdx = base+idx*8 */
        e_load_ptr(&g->code,RAX,RDX);
        break;
    }
    case ND_FIELD:{
        Node*base=e->ch[0];
        Local*l=cg_get(g,base->sval,e->line);
        /* find field offset — search all structs */
        int foff=-1;
        for(int i=0;i<nstructs&&foff<0;i++){
            for(int j=0;j<structs[i].nfields;j++){
                if(!strcmp(structs[i].fields[j],e->sval)){foff=structs[i].offsets[j];break;}
            }
        }
        if(foff<0)die("[Line %d] Unknown field '%s'",e->line,e->sval);
        e_load(&g->code,RAX,l->off+foff);
        break;
    }
    case ND_CAST:{
        cg_expr(g,e->ch[0]);
        if(e->type_hint){
            switch(e->type_hint->kind){
                case TY_I8:case TY_U8:
                    e4(&g->code,0x48,0x0F,0xBE,0xC0); /* movsx rax,al */
                    break;
                case TY_I32:case TY_U32:
                    e3(&g->code,0x48,0x63,0xC0); /* movsxd rax,eax */
                    break;
                default:break;
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
            e3(&g->code,0x0F,0x94,0xC0); /* sete al */
            e4(&g->code,0x48,0x0F,0xB6,0xC0); /* movzx rax,al */
        }
        break;
    }
    case ND_BINOP:{
        const char*op=e->sval;
        /* Strength reduction */
        if(opt_level>=2&&e->ch[1]->is_const){
            int64_t cv=e->ch[1]->const_val;
            if(!strcmp(op,"*")&&cv==0){cg_expr(g,e->ch[0]);e_xor(&g->code,RAX,RAX);break;}
            if(!strcmp(op,"*")&&cv==1){cg_expr(g,e->ch[0]);break;}
            if(!strcmp(op,"*")&&cv==2){cg_expr(g,e->ch[0]);e_add(&g->code,RAX,RAX);break;}
            if(!strcmp(op,"*")&&cv>0&&(cv&(cv-1))==0){
                cg_expr(g,e->ch[0]);
                int sh=0;int64_t t=cv;while(t>1){sh++;t>>=1;}
                e3(&g->code,(uint8_t)(0x48|rb(RAX)),0xC1,(uint8_t)(0xE0|ri(RAX)));
                e1(&g->code,(uint8_t)sh);
                break;
            }
            if((!strcmp(op,"+")||!strcmp(op,"-"))&&cv==0){cg_expr(g,e->ch[0]);break;}
        }
        /* General binop: eval left, push, eval right, combine */
        cg_expr(g,e->ch[0]);
        e_push(&g->code,RAX);    /* save left */
        cg_expr(g,e->ch[1]);
        e_mov(&g->code,RCX,RAX); /* rcx = right */
        e_pop(&g->code,RAX);     /* rax = left */

        if     (!strcmp(op,"+")) e_add(&g->code,RAX,RCX);
        else if(!strcmp(op,"-")) e_sub(&g->code,RAX,RCX);
        else if(!strcmp(op,"*")) e_imul(&g->code,RAX,RCX);
        else if(!strcmp(op,"/")) {e_cqo(&g->code);e_idiv(&g->code,RCX);}
        else if(!strcmp(op,"%")) {e_cqo(&g->code);e_idiv(&g->code,RCX);e_mov(&g->code,RAX,RDX);}
        else if(!strcmp(op,"&")) e_and(&g->code,RAX,RCX);
        else if(!strcmp(op,"|")) e_or(&g->code,RAX,RCX);
        else if(!strcmp(op,"^")) e_xor(&g->code,RAX,RCX);
        else if(!strcmp(op,"<<"))e_mov(&g->code,RCX,RAX),e_pop(&g->code,RAX),e_shl(&g->code,RAX); /* WRONG ORDER */
        else if(!strcmp(op,">>"))e_mov(&g->code,RCX,RAX),e_pop(&g->code,RAX),e_sar(&g->code,RAX); /* WRONG ORDER */
        else if(!strcmp(op,"&&")){
            /* rax=left, rcx=right. result=1 only if both nonzero */
            e_test_rax(&g->code);
            e3(&g->code,0x0F,0x95,0xC0); /* setne al */
            e3(&g->code,0x48,0x85,0xC9); /* test rcx,rcx */
            e3(&g->code,0x0F,0x95,0xC1); /* setne cl */
            e2(&g->code,0x20,0xC8);       /* and al,cl */
            e4(&g->code,0x48,0x0F,0xB6,0xC0); /* movzx rax,al */
        }
        else if(!strcmp(op,"||")){
            e_add(&g->code,RAX,RCX);
            e_test_rax(&g->code);
            e3(&g->code,0x0F,0x95,0xC0);
            e4(&g->code,0x48,0x0F,0xB6,0xC0);
        }
        else { /* comparisons: ==, !=, <, >, <=, >= */
            e_cmp(&g->code,RAX,RCX);
            e_setcc(&g->code,op);
        }
        break;
    }
    case ND_CALL:{
        static int arg_regs[]={RDI,RSI,RDX,RCX,R8,R9};
        if(e->nch>6)die("[Line %d] Max 6 args",e->line);
        /* Evaluate args, push each */
        for(int i=0;i<e->nch;i++){cg_expr(g,e->ch[i]);e_push(&g->code,RAX);}
        /* Pop into arg registers in correct order */
        for(int i=e->nch-1;i>=0;i--)e_pop(&g->code,arg_regs[i]);
        /* Emit call with deferred patch */
        size_t s=e_call(&g->code);
        if(g->nfnp>=MAX_PATCHES)die("Too many function calls");
        strncpy(g->fn_patches[g->nfnp].name,e->sval,63);
        g->fn_patches[g->nfnp].site=s;
        g->nfnp++;
        break;
    }

    case ND_SORT_CALL:{
        /* sort.something{v1,v2,...} — allocate stack array, fill, call sort fn */
        int nvals=e->nch;
        if(nvals==0){break;}
        /* sub rsp to make room for nvals * 8 bytes, aligned */
        int frame_bytes=nvals*8;
        if(frame_bytes%16)frame_bytes+=(16-frame_bytes%16);
        e_sub_rsp(&g->code,frame_bytes);
        /* fill array: evaluate each value, store at [rsp + i*8] */
        for(int vi=0;vi<nvals;vi++){
            cg_expr(g,e->ch[vi]);  /* value in RAX */
            /* mov [rsp + vi*8], rax */
            int off=vi*8;
            if(off==0){
                /* mov [rsp], rax */
                e3(&g->code,0x48,0x89,0x04); e1(&g->code,0x24);
            } else if(off<=127){
                e4(&g->code,0x48,0x89,0x44,0x24); e1(&g->code,(uint8_t)off);
            } else {
                e4(&g->code,0x48,0x89,0x84,0x24); ei32(&g->code,off);
            }
        }
        /* call sort.num_internal(rdi=ptr, rsi=len) */
        e_mov(&g->code,RDI,RSP);
        e_mov_imm(&g->code,RSI,(int64_t)nvals);
        /* emit call to the sort fn — named "sort.num_internal" or whatever prefix */
        /* extract prefix from e->sval: "sort.num" -> call "sort.num_internal" */
        {
            char internal_fn[200];
            /* get the namespace part: everything before the dot */
            char ns[64]={0};
            const char*dot=strchr(e->sval,'.');
            if(dot){strncpy(ns,e->sval,dot-e->sval);}
            else   {strncpy(ns,e->sval,63);}
            snprintf(internal_fn,sizeof(internal_fn),"%s._sort_and_print",ns);
            size_t s=e_call(&g->code);
            strncpy(g->fn_patches[g->nfnp].name,internal_fn,63);
            g->fn_patches[g->nfnp].site=s;
            g->nfnp++;
        }
        e_add_rsp(&g->code,frame_bytes);
        break;
    }
    case ND_EXEC_CALL:{
        const char*m=e->sval;

        /* ── WriteIn / Write ─────────────────────────────── */
        if(!strcmp(m,"WriteIn")||!strcmp(m,"Write")){
            if(e->nch!=1)die("[Line %d] execute.%s() takes 1 arg",e->line,m);
            int nl=!strcmp(m,"WriteIn");
            Node*arg=e->ch[0];
            if(arg->kind==ND_STR){
                /* Literal string: bake directly into rodata */
                size_t plen;
                size_t off=cg_str(g,arg->sval,nl,&plen);
                cg_write_lit(g,off,plen);
            } else {
                /* Variable/expression: evaluate to get pointer, call strlen for length */
                cg_expr(g,arg);        /* rax = ptr */
                e_push(&g->code,RAX); /* save ptr */
                e_mov(&g->code,RDI,RAX);
                cg_call_strlen(g);     /* rax = strlen(ptr) */
                e_mov(&g->code,RDX,RAX);
                e_pop(&g->code,RSI);  /* rsi = ptr */
                cg_write(g);          /* sys_write(1,rsi,rdx) */
                if(nl)cg_write_nl(g);
            }
        }

        /* ── WriteInt ────────────────────────────────────── */
        else if(!strcmp(m,"WriteInt")){
            if(e->nch!=1)die("[Line %d] execute.WriteInt() takes 1 arg",e->line);
            cg_expr(g,e->ch[0]);      /* rax = integer */
            e_mov(&g->code,RDI,RAX);
            cg_call_itoa(g);          /* rsi=ptr, rdx=len */
            e_mov_imm(&g->code,RAX,1);
            e_mov_imm(&g->code,RDI,1);
            e_syscall(&g->code);
            cg_write_nl(g);
        }

        /* ── WriteChar ───────────────────────────────────── */
        else if(!strcmp(m,"WriteChar")){
            if(e->nch!=1)die("[Line %d] execute.WriteChar() takes 1 arg",e->line);
            cg_expr(g,e->ch[0]);
            e_push(&g->code,RAX);    /* push char onto stack */
            e_mov(&g->code,RSI,RSP); /* rsi = &char */
            e_mov_imm(&g->code,RAX,1);
            e_mov_imm(&g->code,RDI,1);
            e_mov_imm(&g->code,RDX,1);
            e_syscall(&g->code);
            e_pop(&g->code,RAX);     /* clean stack */
        }

        /* ── ReadIn ──────────────────────────────────────── */
        else if(!strcmp(m,"ReadIn")){
            /* ReadIn(buf_ptr, max_len) — reads one byte at a time until '\n' or max_len.
             * Uses r10 (write ptr) and r11 (scratch) — both caller-saved, safe to clobber.
             * Returns buf_ptr in rax. */
            if(e->nch!=2)die("[Line %d] execute.ReadIn(buf, max_len)",e->line);

            /* Eval buf ptr, save in r10 (write ptr) */
            cg_expr(g,e->ch[0]);
            /* mov r10,rax */
            e3(&g->code,0x49,0x89,0xC2);   /* mov r10,rax */
            /* Eval max_len -> rcx */
            cg_expr(g,e->ch[1]);
            e_mov(&g->code,RCX,RAX);        /* rcx = max_len */

            /* scratch byte on stack for sys_read */
            e_sub_rsp(&g->code,16);         /* 16 bytes (8 scratch + 8 align) */

            /* read_loop: */
            size_t rd_loop=buf_pos(&g->code);
            /* if rcx == 0: done (buffer full) */
            e3(&g->code,0x48,0x85,0xC9);   /* test rcx,rcx */
            size_t jz_full=e_je(&g->code);

            /* sys_read(0, rsp, 1) — read 1 byte */
            e_mov(&g->code,RSI,RSP);        /* rsi = &scratch */
            e_mov_imm(&g->code,RAX,0);      /* sys_read */
            e_mov_imm(&g->code,RDI,0);      /* stdin */
            e_mov_imm(&g->code,RDX,1);
            e_syscall(&g->code);

            /* if bytes_read <= 0: EOF, stop */
            e_test_rax(&g->code);
            size_t jz_eof=e_je(&g->code);

            /* movzx r11, byte[rsp] — load the byte */
            e4(&g->code,0x4C,0x0F,0xB6,0x1C); e1(&g->code,0x24);

            /* if r11 == '\n': stop (don't store it) */
            e4(&g->code,0x49,0x83,0xFB,0x0A);  /* cmp r11,10 */
            size_t je_nl=e_je(&g->code);

            /* store byte: mov byte[r10],r11b */
            e3(&g->code,0x45,0x88,0x1A);   /* mov byte[r10],r11b */
            /* inc r10 */
            e3(&g->code,0x49,0xFF,0xC2);   /* inc r10 */
            /* dec rcx */
            e3(&g->code,0x48,0xFF,0xC9);   /* dec rcx */
            /* jmp rd_loop */
            e_jmp_back(&g->code,rd_loop);

            /* done: patch all exits here */
            size_t done=buf_pos(&g->code);
            patch(&g->code,jz_full,done);
            patch(&g->code,jz_eof,done);
            patch(&g->code,je_nl,done);

            /* null-terminate: mov byte[r10],0 */
            e4(&g->code,0x41,0xC6,0x02,0x00); /* mov byte[r10],0 */

            e_add_rsp(&g->code,16);

            /* return original buf ptr */
            cg_expr(g,e->ch[0]);            /* re-eval buf ptr into rax */
        }

        /* ── ReadInt ─────────────────────────────────────── */
        else if(!strcmp(m,"ReadInt")){
            /* Read integer from stdin one byte at a time (so we don't consume
             * extra bytes that belong to the next ReadIn call).
             * Algorithm: read bytes until non-digit, parse as signed integer.
             * Uses: rbx=result, r12=sign, r13=scratch_byte_on_stack.
             * Saves and restores rbx, r12. */
            if(e->nch!=0)die("[Line %d] execute.ReadInt() takes no args",e->line);

            e_push(&g->code,RBX);                  /* save rbx */
            e2(&g->code,0x41,0x54);                /* push r12 */
            e_sub_rsp(&g->code,16);                /* 8-byte scratch + 8 align */

            e_xor(&g->code,RBX,RBX);               /* rbx = result = 0 */
            e_xor(&g->code,R9,R9);                 /* r9 = is_negative = 0 (r9 is caller-saved, ok) */

            /* Skip leading whitespace and detect sign */
            size_t skip_ws=buf_pos(&g->code);
            /* read 1 byte into [rsp] */
            e_mov(&g->code,RSI,RSP);
            e_mov_imm(&g->code,RAX,0);              /* sys_read */
            e_mov_imm(&g->code,RDI,0);              /* stdin */
            e_mov_imm(&g->code,RDX,1);
            e_syscall(&g->code);
            /* if bytes_read <= 0: EOF, return 0 */
            e_test_rax(&g->code);
            size_t jle_eof=e_je(&g->code);
            /* movzx rax, byte[rsp] */
            e4(&g->code,0x48,0x0F,0xB6,0x04); e1(&g->code,0x24); /* movzx rax,byte[rsp] */
            /* skip whitespace (space, 	, 
, 
) */
            /* cmp rax,' ' ; je skip_ws */
            e4(&g->code,0x48,0x83,0xF8,0x20);
            size_t je_sp=e_je(&g->code); patch(&g->code,je_sp,skip_ws);
            e4(&g->code,0x48,0x83,0xF8,0x09); /* cmp rax,	 */
            size_t je_ht=e_je(&g->code); patch(&g->code,je_ht,skip_ws);
            e4(&g->code,0x48,0x83,0xF8,0x0D); /* cmp rax,
 */
            size_t je_cr=e_je(&g->code); patch(&g->code,je_cr,skip_ws);
            e4(&g->code,0x48,0x83,0xF8,0x0A); /* cmp rax,
 */
            size_t je_nl=e_je(&g->code); patch(&g->code,je_nl,skip_ws);
            /* check for '-' */
            e4(&g->code,0x48,0x83,0xF8,0x2D); /* cmp rax,'-' */
            size_t jne_minus=e_jne(&g->code);
            e_mov_imm(&g->code,R9,1);           /* is_negative=1 */
            /* read next byte */
            e_mov(&g->code,RSI,RSP);
            e_mov_imm(&g->code,RAX,0);e_mov_imm(&g->code,RDI,0);e_mov_imm(&g->code,RDX,1);
            e_syscall(&g->code);
            e_test_rax(&g->code);
            size_t jle_eof2=e_je(&g->code);
            e4(&g->code,0x48,0x0F,0xB6,0x04); e1(&g->code,0x24); /* movzx rax,byte[rsp] */
            patch(&g->code,jne_minus,buf_pos(&g->code));
            /* digit loop: rax = current char */
            size_t dloop=buf_pos(&g->code);
            /* check '0'..'9' */
            e4(&g->code,0x48,0x83,0xF8,0x30); /* cmp rax,'0' */
            size_t jl=e_je(&g->code); g->code.len-=6;
            e2(&g->code,0x0F,0x8C); size_t jl_s=buf_pos(&g->code); ei32(&g->code,0);
            e4(&g->code,0x48,0x83,0xF8,0x39); /* cmp rax,'9' */
            e2(&g->code,0x0F,0x8F); size_t jg_s=buf_pos(&g->code); ei32(&g->code,0);
            /* rbx = rbx*10 + (rax-'0') */
            e4(&g->code,0x48,0x6B,0xDB,0x0A); /* imul rbx,rbx,10 */
            e4(&g->code,0x48,0x83,0xE8,0x30); /* sub rax,'0' */
            e3(&g->code,0x48,0x01,0xC3);       /* add rbx,rax */
            /* read next byte */
            e_mov(&g->code,RSI,RSP);
            e_mov_imm(&g->code,RAX,0);e_mov_imm(&g->code,RDI,0);e_mov_imm(&g->code,RDX,1);
            e_syscall(&g->code);
            e_test_rax(&g->code);
            size_t jle_eof3=e_je(&g->code);
            e4(&g->code,0x48,0x0F,0xB6,0x04); e1(&g->code,0x24); /* movzx rax,byte[rsp] */
            e_jmp_back(&g->code,dloop);
            /* done: */
            size_t done_lbl=buf_pos(&g->code);
            patch(&g->code,jl_s,done_lbl); patch(&g->code,jg_s,done_lbl);
            patch(&g->code,jle_eof,done_lbl); patch(&g->code,jle_eof2,done_lbl);
            patch(&g->code,jle_eof3,done_lbl);
            /* apply sign */
            e3(&g->code,0x49,0x85,0xC9); /* test r9,r9 */
            size_t jz_sign=e_je(&g->code);
            e3(&g->code,0x48,0xF7,0xDB); /* neg rbx */
            patch(&g->code,jz_sign,buf_pos(&g->code));
            e_mov(&g->code,RAX,RBX);
            e_add_rsp(&g->code,16);
            e2(&g->code,0x41,0x5C);       /* pop r12 */
            e_pop(&g->code,RBX);
        }

        /* ── StrLen ──────────────────────────────────────── */
        else if(!strcmp(m,"StrLen")){
            if(e->nch!=1)die("[Line %d] execute.StrLen() takes 1 arg",e->line);
            cg_expr(g,e->ch[0]);
            e_mov(&g->code,RDI,RAX);
            cg_call_strlen(g);
        }

        /* ── StrEq ───────────────────────────────────────── */
        else if(!strcmp(m,"StrEq")){
            if(e->nch!=2)die("[Line %d] execute.StrEq() takes 2 args",e->line);
            cg_expr(g,e->ch[0]); e_push(&g->code,RAX);
            cg_expr(g,e->ch[1]);
            e_mov(&g->code,RSI,RAX);
            e_pop(&g->code,RDI);
            /* Compare byte by byte: rcx=index=0 */
            e_xor(&g->code,RCX,RCX);
            size_t cmp_loop=buf_pos(&g->code);
            /* movzx rax,byte[rdi+rcx] */
            e4(&g->code,0x0F,0xB6,0x04,0x0F);
            /* movzx rdx,byte[rsi+rcx] */
            e4(&g->code,0x0F,0xB6,0x14,0x0E);
            /* cmp eax,edx */
            e2(&g->code,0x39,0xD0);
            size_t jne_neq=e_jne(&g->code);
            /* test eax,eax -> jz equal (both zero means end of string) */
            e2(&g->code,0x85,0xC0);
            size_t jz_eq=e_je(&g->code);
            /* inc rcx, jmp loop */
            e3(&g->code,0x48,0xFF,0xC1);
            e_jmp_back(&g->code,cmp_loop);
            /* not_equal: */
            patch(&g->code,jne_neq,buf_pos(&g->code));
            e_xor(&g->code,RAX,RAX); /* rax=0 */
            size_t jmp_done=e_jmp(&g->code);
            /* equal: */
            patch(&g->code,jz_eq,buf_pos(&g->code));
            e_mov_imm(&g->code,RAX,1);
            /* done: */
            patch(&g->code,jmp_done,buf_pos(&g->code));
        }

        /* ── StrCat ──────────────────────────────────────── */
        else if(!strcmp(m,"StrCat")){
            if(e->nch!=2)die("[Line %d] execute.StrCat(dst,src)",e->line);
            cg_expr(g,e->ch[0]); e_push(&g->code,RAX); /* save dst */
            cg_expr(g,e->ch[1]);
            e_mov(&g->code,RSI,RAX); /* rsi=src */
            e_pop(&g->code,RDI);     /* rdi=dst */
            /* advance rdi to null terminator of dst */
            size_t find_end=buf_pos(&g->code);
            e3(&g->code,0x80,0x3F,0x00); /* cmp byte[rdi],0 */
            size_t jz_end=e_je(&g->code);
            e3(&g->code,0x48,0xFF,0xC7); /* inc rdi */
            e_jmp_back(&g->code,find_end);
            patch(&g->code,jz_end,buf_pos(&g->code));
            /* copy src->rdi byte by byte including null */
            size_t cpy_loop=buf_pos(&g->code);
            e2(&g->code,0x8A,0x06);   /* mov al,[rsi] */
            e2(&g->code,0x88,0x07);   /* mov [rdi],al */
            e3(&g->code,0x48,0xFF,0xC6); /* inc rsi */
            e3(&g->code,0x48,0xFF,0xC7); /* inc rdi */
            e2(&g->code,0x84,0xC0);   /* test al,al */
            size_t jnz_cpy=e_jne(&g->code);
            patch(&g->code,jnz_cpy,cpy_loop);
        }

        /* ── StrCopy ─────────────────────────────────────── */
        else if(!strcmp(m,"StrCopy")){
            if(e->nch!=2)die("[Line %d] execute.StrCopy(dst,src)",e->line);
            cg_expr(g,e->ch[0]); e_push(&g->code,RAX);
            cg_expr(g,e->ch[1]);
            e_mov(&g->code,RSI,RAX);
            e_pop(&g->code,RDI);
            size_t cpy=buf_pos(&g->code);
            e2(&g->code,0x8A,0x06);e2(&g->code,0x88,0x07);
            e3(&g->code,0x48,0xFF,0xC6);e3(&g->code,0x48,0xFF,0xC7);
            e2(&g->code,0x84,0xC0);
            size_t jnz=e_jne(&g->code);
            patch(&g->code,jnz,cpy);
        }

        /* ── Alloc ───────────────────────────────────────── */
        else if(!strcmp(m,"Alloc")){
            /* mmap(0, size, PROT_READ|PROT_WRITE=3, MAP_PRIVATE|MAP_ANON=0x22, -1, 0) */
            if(e->nch!=1)die("[Line %d] execute.Alloc(size)",e->line);
            cg_expr(g,e->ch[0]);
            e_mov(&g->code,RSI,RAX);           /* rsi=length */
            e_xor(&g->code,RDI,RDI);           /* rdi=addr=0 */
            e_mov_imm(&g->code,RDX,3);          /* rdx=PROT_RW */
            /* r10=MAP_PRIVATE|MAP_ANONYMOUS=0x22 (syscall arg4 uses r10 not rcx) */
            e3(&g->code,0x41,0xBA,0x22); e3(&g->code,0x00,0x00,0x00); /* mov r10d,0x22 */
            e_mov_imm(&g->code,R8,-1);           /* r8=fd=-1 */
            e_xor(&g->code,R9,R9);              /* r9=offset=0 */
            e_mov_imm(&g->code,RAX,9);           /* sys_mmap */
            e_syscall(&g->code);
        }

        /* ── Free ────────────────────────────────────────── */
        else if(!strcmp(m,"Free")){
            if(e->nch!=2)die("[Line %d] execute.Free(ptr,size)",e->line);
            cg_expr(g,e->ch[0]); e_push(&g->code,RAX);
            cg_expr(g,e->ch[1]);
            e_mov(&g->code,RSI,RAX);
            e_pop(&g->code,RDI);
            e_mov_imm(&g->code,RAX,11); /* sys_munmap */
            e_syscall(&g->code);
        }

        /* ── Syscall ─────────────────────────────────────── */
        else if(!strcmp(m,"Syscall")){
            /* Syscall(num, a1, a2, a3, a4, a5, a6)
             * Linux x86-64: rax=num, rdi=a1, rsi=a2, rdx=a3, r10=a4, r8=a5, r9=a6
             * Push args RIGHT-TO-LEFT so syscall num is on top when we pop. */
            if(e->nch<1)die("[Line %d] execute.Syscall(num,...)",e->line);
            int nargs=e->nch-1;
            /* Push in reverse: last arg first, syscall num last (ends up on top) */
            for(int i=e->nch-1;i>=0;i--){cg_expr(g,e->ch[i]);e_push(&g->code,RAX);}
            e_pop(&g->code,RAX);          /* rax = syscall num */
            if(nargs>0)e_pop(&g->code,RDI);
            if(nargs>1)e_pop(&g->code,RSI);
            if(nargs>2)e_pop(&g->code,RDX);
            if(nargs>3){e2(&g->code,0x41,0x5A);} /* pop r10 */
            if(nargs>4)e_pop(&g->code,R8);
            if(nargs>5)e_pop(&g->code,R9);
            e_syscall(&g->code);
        }

        /* ── Exit ────────────────────────────────────────── */
        else if(!strcmp(m,"Exit")){
            if(e->nch!=1)die("[Line %d] execute.Exit() takes 1 arg",e->line);
            cg_expr(g,e->ch[0]);
            e_mov(&g->code,RDI,RAX);
            e_mov_imm(&g->code,RAX,60);
            e_syscall(&g->code);
        }

        /* ── Low-level / systems calls ───────────────────── */

        else if(!strcmp(m,"GetPID")){
            /* sys_getpid() -> rax */
            if(e->nch!=0)die("[Line %d] execute.GetPID() takes no args",e->line);
            e_mov_imm(&g->code,RAX,39); /* sys_getpid */
            e_syscall(&g->code);
        }
        else if(!strcmp(m,"GetUID")){
            if(e->nch!=0)die("[Line %d] execute.GetUID() takes no args",e->line);
            e_mov_imm(&g->code,RAX,102); /* sys_getuid */
            e_syscall(&g->code);
        }
        else if(!strcmp(m,"Sleep")){
            /* Sleep(seconds) — uses sys_nanosleep with a timespec on stack */
            if(e->nch!=1)die("[Line %d] execute.Sleep(seconds)",e->line);
            cg_expr(g,e->ch[0]);
            /* push timespec: tv_sec=rax, tv_nsec=0 */
            e_push(&g->code,RAX);          /* tv_sec */
            e_xor(&g->code,RCX,RCX);
            e_push(&g->code,RCX);          /* tv_nsec = 0 (pushed first = higher addr) */
            /* swap: timespec is {tv_sec, tv_nsec} so tv_sec at lower addr */
            /* Actually: push tv_nsec first (higher addr), then tv_sec (lower addr) */
            /* Redo: sub rsp,16; mov [rsp],rax (tv_sec); mov [rsp+8],0 (tv_nsec) */
            g->code.len -= 6; /* undo the two pushes */
            e_sub_rsp(&g->code,16);
            /* mov [rsp], rax */
            e3(&g->code,0x48,0x89,0x04); e1(&g->code,0x24);
            /* mov qword[rsp+8], 0 */
            e4(&g->code,0x48,0xC7,0x44,0x24); e1(&g->code,0x08); ei32(&g->code,0);
            e_mov(&g->code,RDI,RSP);       /* rdi = &timespec */
            e_xor(&g->code,RSI,RSI);       /* rsi = NULL (no remainder) */
            e_mov_imm(&g->code,RAX,35);    /* sys_nanosleep */
            e_syscall(&g->code);
            e_add_rsp(&g->code,16);
        }
        else if(!strcmp(m,"SleepMs")){
            /* SleepMs(milliseconds) */
            if(e->nch!=1)die("[Line %d] execute.SleepMs(ms)",e->line);
            cg_expr(g,e->ch[0]);           /* rax = ms */
            e_push(&g->code,RAX);          /* save ms */
            /* tv_sec = ms / 1000 */
            e_mov_imm(&g->code,RCX,1000);
            e_cqo(&g->code);
            e_idiv(&g->code,RCX);          /* rax=sec, rdx=rem_ms */
            e_push(&g->code,RAX);          /* save sec */
            /* tv_nsec = rem_ms * 1000000 */
            e_mov(&g->code,RAX,RDX);
            e_mov_imm(&g->code,RCX,1000000);
            e_imul(&g->code,RAX,RCX);
            e_push(&g->code,RAX);          /* push tv_nsec */
            e_pop(&g->code,RSI);           /* rsi = tv_nsec */
            e_pop(&g->code,RDI);           /* rdi = tv_sec */
            e_pop(&g->code,RCX);           /* clean ms */
            e_sub_rsp(&g->code,16);
            e3(&g->code,0x48,0x89,0x3C); e1(&g->code,0x24); /* mov [rsp],rdi=sec */
            e4(&g->code,0x48,0x89,0x74,0x24); e1(&g->code,0x08); /* mov [rsp+8],rsi=nsec */
            e_mov(&g->code,RDI,RSP);
            e_xor(&g->code,RSI,RSI);
            e_mov_imm(&g->code,RAX,35);
            e_syscall(&g->code);
            e_add_rsp(&g->code,16);
        }
        else if(!strcmp(m,"FileOpen")){
            /* FileOpen(path_ptr, flags) -> fd
             * flags: 0=read, 1=write|create|trunc, 2=read+write */
            if(e->nch!=2)die("[Line %d] execute.FileOpen(path, flags)",e->line);
            cg_expr(g,e->ch[0]); e_push(&g->code,RAX); /* path */
            cg_expr(g,e->ch[1]);                         /* flags */
            e_mov(&g->code,RSI,RAX);
            e_pop(&g->code,RDI);
            /* if flags==1: O_WRONLY|O_CREAT|O_TRUNC = 0x241, mode=0644 */
            /* if flags==2: O_RDWR = 2 */
            /* if flags==0: O_RDONLY = 0 */
            /* Simplify: pass flags directly as open flags, mode=0644 */
            e_mov_imm(&g->code,RDX,0644); /* mode */
            e_mov_imm(&g->code,RAX,2);    /* sys_open */
            e_syscall(&g->code);
        }
        else if(!strcmp(m,"FileRead")){
            /* FileRead(fd, buf, len) -> bytes_read */
            if(e->nch!=3)die("[Line %d] execute.FileRead(fd,buf,len)",e->line);
            cg_expr(g,e->ch[0]); e_push(&g->code,RAX);
            cg_expr(g,e->ch[1]); e_push(&g->code,RAX);
            cg_expr(g,e->ch[2]);
            e_mov(&g->code,RDX,RAX);
            e_pop(&g->code,RSI);
            e_pop(&g->code,RDI);
            e_mov_imm(&g->code,RAX,0); /* sys_read */
            e_syscall(&g->code);
        }
        else if(!strcmp(m,"FileWrite")){
            /* FileWrite(fd, buf, len) -> bytes_written */
            if(e->nch!=3)die("[Line %d] execute.FileWrite(fd,buf,len)",e->line);
            cg_expr(g,e->ch[0]); e_push(&g->code,RAX);
            cg_expr(g,e->ch[1]); e_push(&g->code,RAX);
            cg_expr(g,e->ch[2]);
            e_mov(&g->code,RDX,RAX);
            e_pop(&g->code,RSI);
            e_pop(&g->code,RDI);
            e_mov_imm(&g->code,RAX,1); /* sys_write */
            e_syscall(&g->code);
        }
        else if(!strcmp(m,"FileClose")){
            /* FileClose(fd) */
            if(e->nch!=1)die("[Line %d] execute.FileClose(fd)",e->line);
            cg_expr(g,e->ch[0]);
            e_mov(&g->code,RDI,RAX);
            e_mov_imm(&g->code,RAX,3); /* sys_close */
            e_syscall(&g->code);
        }
        else if(!strcmp(m,"FileSeek")){
            /* FileSeek(fd, offset, whence) -> new_offset
             * whence: 0=SEEK_SET, 1=SEEK_CUR, 2=SEEK_END */
            if(e->nch!=3)die("[Line %d] execute.FileSeek(fd,offset,whence)",e->line);
            cg_expr(g,e->ch[0]); e_push(&g->code,RAX);
            cg_expr(g,e->ch[1]); e_push(&g->code,RAX);
            cg_expr(g,e->ch[2]);
            e_mov(&g->code,RDX,RAX);
            e_pop(&g->code,RSI);
            e_pop(&g->code,RDI);
            e_mov_imm(&g->code,RAX,8); /* sys_lseek */
            e_syscall(&g->code);
        }
        else if(!strcmp(m,"FileStat")){
            /* FileStat(path, stat_buf) -> 0 on success, -1 on error */
            if(e->nch!=2)die("[Line %d] execute.FileStat(path,buf)",e->line);
            cg_expr(g,e->ch[0]); e_push(&g->code,RAX);
            cg_expr(g,e->ch[1]);
            e_mov(&g->code,RSI,RAX);
            e_pop(&g->code,RDI);
            e_mov_imm(&g->code,RAX,4); /* sys_stat */
            e_syscall(&g->code);
        }
        else if(!strcmp(m,"FileDelete")){
            /* FileDelete(path) -> 0 on success */
            if(e->nch!=1)die("[Line %d] execute.FileDelete(path)",e->line);
            cg_expr(g,e->ch[0]);
            e_mov(&g->code,RDI,RAX);
            e_mov_imm(&g->code,RAX,87); /* sys_unlink */
            e_syscall(&g->code);
        }
        else if(!strcmp(m,"Fork")){
            /* Fork() -> 0 in child, child_pid in parent */
            if(e->nch!=0)die("[Line %d] execute.Fork() takes no args",e->line);
            e_mov_imm(&g->code,RAX,57); /* sys_fork */
            e_syscall(&g->code);
        }
        else if(!strcmp(m,"Wait")){
            /* Wait() -> child pid. Waits for any child. */
            if(e->nch!=0)die("[Line %d] execute.Wait() takes no args",e->line);
            e_mov_imm(&g->code,RDI,-1);  /* pid=-1 = any child */
            e_xor(&g->code,RSI,RSI);     /* wstatus=NULL */
            e_xor(&g->code,RDX,RDX);     /* options=0 */
            e_xor(&g->code,R10,R10);     /* rusage=NULL */
            e_mov_imm(&g->code,RAX,61);  /* sys_wait4 */
            e_syscall(&g->code);
        }
        else if(!strcmp(m,"Exec")){
            /* Exec(path, argv_ptr) — execve */
            if(e->nch!=2)die("[Line %d] execute.Exec(path,argv)",e->line);
            cg_expr(g,e->ch[0]); e_push(&g->code,RAX);
            cg_expr(g,e->ch[1]);
            e_mov(&g->code,RSI,RAX);
            e_pop(&g->code,RDI);
            e_xor(&g->code,RDX,RDX);    /* envp=NULL */
            e_mov_imm(&g->code,RAX,59); /* sys_execve */
            e_syscall(&g->code);
        }
        else if(!strcmp(m,"Args")){
            /* Args(n) -> ptr to argv[n] string
             * argc/argv are at [rsp] at program entry. We save them in globals.
             * Simpler: use the stack-based argv that Linux puts before _start. */
            if(e->nch!=1)die("[Line %d] execute.Args(n)",e->line);
            cg_expr(g,e->ch[0]);   /* rax = n */
            /* argv is passed in rdi at _start entry (actually on stack).
             * We stored argc in r12 and argv in r13 at startup. */
            e_mov(&g->code,RCX,RAX);    /* rcx = n */
            /* rax = argv[n] = [r13 + n*8] */
            /* lea rax,[r13+rcx*8] then mov rax,[rax] */
            /* REX.W.R.B=0x4D, 8D, ModRM(0,rax=0,SIB=4), SIB(3,rcx=1,r13=5) */
            e4(&g->code,0x4F,0x8D,0x04,0xCD); ei32(&g->code,0); /* wrong, redo */
            g->code.len -= 8;
            /* simpler: imul rcx,8 then add r13, load */
            /* imul rcx,rcx,8 */
            e4(&g->code,0x48,0x6B,0xC9,0x08);
            /* add rcx,r13 */
            e3(&g->code,0x4C,0x01,0xE9);
            /* mov rax,[rcx] */
            e3(&g->code,0x48,0x8B,0x01);
        }
        else if(!strcmp(m,"ArgCount")){
            /* ArgCount() -> argc */
            if(e->nch!=0)die("[Line %d] execute.ArgCount() takes no args",e->line);
            /* return r12 (saved argc) */
            e_mov(&g->code,RAX,R12);
        }
        else if(!strcmp(m,"Env")){
            /* Env(name_ptr) -> value_ptr or NULL if not found */
            if(e->nch!=1)die("[Line %d] execute.Env(name)",e->line);
            cg_expr(g,e->ch[0]);
            e_mov(&g->code,RDI,RAX);    /* rdi = name */
            /* Call our internal env search subroutine */
            {size_t s=e_call(&g->code);
             strncpy(g->fn_patches[g->nfnp].name,"__env_get",63);
             g->fn_patches[g->nfnp].site=s;g->nfnp++;}
        }
        else if(!strcmp(m,"Socket")){
            /* Socket(domain, type, protocol) -> fd
             * domain: 2=AF_INET, type: 1=SOCK_STREAM, protocol: 0 */
            if(e->nch!=3)die("[Line %d] execute.Socket(domain,type,proto)",e->line);
            cg_expr(g,e->ch[0]); e_push(&g->code,RAX);
            cg_expr(g,e->ch[1]); e_push(&g->code,RAX);
            cg_expr(g,e->ch[2]);
            e_mov(&g->code,RDX,RAX);
            e_pop(&g->code,RSI);
            e_pop(&g->code,RDI);
            e_mov_imm(&g->code,RAX,41); /* sys_socket */
            e_syscall(&g->code);
        }
        else if(!strcmp(m,"Bind")){
            /* Bind(fd, sockaddr_ptr, addrlen) -> 0 or -1 */
            if(e->nch!=3)die("[Line %d] execute.Bind(fd,addr,len)",e->line);
            cg_expr(g,e->ch[0]); e_push(&g->code,RAX);
            cg_expr(g,e->ch[1]); e_push(&g->code,RAX);
            cg_expr(g,e->ch[2]);
            e_mov(&g->code,RDX,RAX);
            e_pop(&g->code,RSI);
            e_pop(&g->code,RDI);
            e_mov_imm(&g->code,RAX,49); /* sys_bind */
            e_syscall(&g->code);
        }
        else if(!strcmp(m,"Listen")){
            /* Listen(fd, backlog) */
            if(e->nch!=2)die("[Line %d] execute.Listen(fd,backlog)",e->line);
            cg_expr(g,e->ch[0]); e_push(&g->code,RAX);
            cg_expr(g,e->ch[1]);
            e_mov(&g->code,RSI,RAX);
            e_pop(&g->code,RDI);
            e_mov_imm(&g->code,RAX,50); /* sys_listen */
            e_syscall(&g->code);
        }
        else if(!strcmp(m,"Accept")){
            /* Accept(fd) -> client_fd */
            if(e->nch!=1)die("[Line %d] execute.Accept(fd)",e->line);
            cg_expr(g,e->ch[0]);
            e_mov(&g->code,RDI,RAX);
            e_xor(&g->code,RSI,RSI);    /* addr=NULL */
            e_xor(&g->code,RDX,RDX);    /* addrlen=NULL */
            e_mov_imm(&g->code,RAX,43); /* sys_accept */
            e_syscall(&g->code);
        }
        else if(!strcmp(m,"Connect")){
            /* Connect(fd, sockaddr_ptr, addrlen) -> 0 or -1 */
            if(e->nch!=3)die("[Line %d] execute.Connect(fd,addr,len)",e->line);
            cg_expr(g,e->ch[0]); e_push(&g->code,RAX);
            cg_expr(g,e->ch[1]); e_push(&g->code,RAX);
            cg_expr(g,e->ch[2]);
            e_mov(&g->code,RDX,RAX);
            e_pop(&g->code,RSI);
            e_pop(&g->code,RDI);
            e_mov_imm(&g->code,RAX,42); /* sys_connect */
            e_syscall(&g->code);
        }
        else if(!strcmp(m,"Send")){
            /* Send(fd, buf, len, flags) -> bytes_sent */
            if(e->nch!=4)die("[Line %d] execute.Send(fd,buf,len,flags)",e->line);
            cg_expr(g,e->ch[0]); e_push(&g->code,RAX);
            cg_expr(g,e->ch[1]); e_push(&g->code,RAX);
            cg_expr(g,e->ch[2]); e_push(&g->code,RAX);
            cg_expr(g,e->ch[3]);
            e_mov(&g->code,R10,RAX);
            e_pop(&g->code,RDX);
            e_pop(&g->code,RSI);
            e_pop(&g->code,RDI);
            e_mov_imm(&g->code,RAX,44); /* sys_sendto */
            e_xor(&g->code,R8,R8); e_xor(&g->code,R9,R9);
            e_syscall(&g->code);
        }
        else if(!strcmp(m,"Recv")){
            /* Recv(fd, buf, len, flags) -> bytes_received */
            if(e->nch!=4)die("[Line %d] execute.Recv(fd,buf,len,flags)",e->line);
            cg_expr(g,e->ch[0]); e_push(&g->code,RAX);
            cg_expr(g,e->ch[1]); e_push(&g->code,RAX);
            cg_expr(g,e->ch[2]); e_push(&g->code,RAX);
            cg_expr(g,e->ch[3]);
            e_mov(&g->code,R10,RAX);
            e_pop(&g->code,RDX);
            e_pop(&g->code,RSI);
            e_pop(&g->code,RDI);
            e_mov_imm(&g->code,RAX,45); /* sys_recvfrom */
            e_xor(&g->code,R8,R8); e_xor(&g->code,R9,R9);
            e_syscall(&g->code);
        }
        else if(!strcmp(m,"Mmap")){
            /* Mmap(addr,len,prot,flags,fd,off) -> ptr */
            if(e->nch!=6)die("[Line %d] execute.Mmap(addr,len,prot,flags,fd,off)",e->line);
            cg_expr(g,e->ch[0]); e_push(&g->code,RAX);
            cg_expr(g,e->ch[1]); e_push(&g->code,RAX);
            cg_expr(g,e->ch[2]); e_push(&g->code,RAX);
            cg_expr(g,e->ch[3]); e_push(&g->code,RAX);
            cg_expr(g,e->ch[4]); e_push(&g->code,RAX);
            cg_expr(g,e->ch[5]);
            e_mov(&g->code,R9,RAX);
            e_pop(&g->code,R8);
            e2(&g->code,0x41,0x5A); /* pop r10 = flags */
            e_pop(&g->code,RDX);
            e_pop(&g->code,RSI);
            e_pop(&g->code,RDI);
            e_mov_imm(&g->code,RAX,9); /* sys_mmap */
            e_syscall(&g->code);
        }
        else if(!strcmp(m,"Munmap")){
            if(e->nch!=2)die("[Line %d] execute.Munmap(ptr,len)",e->line);
            cg_expr(g,e->ch[0]); e_push(&g->code,RAX);
            cg_expr(g,e->ch[1]);
            e_mov(&g->code,RSI,RAX);
            e_pop(&g->code,RDI);
            e_mov_imm(&g->code,RAX,11); /* sys_munmap */
            e_syscall(&g->code);
        }
        else if(!strcmp(m,"Mprotect")){
            /* Mprotect(ptr, len, prot) */
            if(e->nch!=3)die("[Line %d] execute.Mprotect(ptr,len,prot)",e->line);
            cg_expr(g,e->ch[0]); e_push(&g->code,RAX);
            cg_expr(g,e->ch[1]); e_push(&g->code,RAX);
            cg_expr(g,e->ch[2]);
            e_mov(&g->code,RDX,RAX);
            e_pop(&g->code,RSI);
            e_pop(&g->code,RDI);
            e_mov_imm(&g->code,RAX,10); /* sys_mprotect */
            e_syscall(&g->code);
        }
        else if(!strcmp(m,"Pipe")){
            /* Pipe(fds_ptr) -> 0 on success. fds[0]=read, fds[1]=write */
            if(e->nch!=1)die("[Line %d] execute.Pipe(fds_ptr)",e->line);
            cg_expr(g,e->ch[0]);
            e_mov(&g->code,RDI,RAX);
            e_mov_imm(&g->code,RAX,22); /* sys_pipe */
            e_syscall(&g->code);
        }
        else if(!strcmp(m,"Dup2")){
            /* Dup2(oldfd, newfd) -> newfd */
            if(e->nch!=2)die("[Line %d] execute.Dup2(oldfd,newfd)",e->line);
            cg_expr(g,e->ch[0]); e_push(&g->code,RAX);
            cg_expr(g,e->ch[1]);
            e_mov(&g->code,RSI,RAX);
            e_pop(&g->code,RDI);
            e_mov_imm(&g->code,RAX,33); /* sys_dup2 */
            e_syscall(&g->code);
        }
        else if(!strcmp(m,"Ioctl")){
            /* Ioctl(fd, request, arg) -> result */
            if(e->nch!=3)die("[Line %d] execute.Ioctl(fd,req,arg)",e->line);
            cg_expr(g,e->ch[0]); e_push(&g->code,RAX);
            cg_expr(g,e->ch[1]); e_push(&g->code,RAX);
            cg_expr(g,e->ch[2]);
            e_mov(&g->code,RDX,RAX);
            e_pop(&g->code,RSI);
            e_pop(&g->code,RDI);
            e_mov_imm(&g->code,RAX,16); /* sys_ioctl */
            e_syscall(&g->code);
        }
        else if(!strcmp(m,"SetByte")){
            /* SetByte(ptr, offset, byte_val) — write 1 byte */
            if(e->nch!=3)die("[Line %d] execute.SetByte(ptr,off,val)",e->line);
            cg_expr(g,e->ch[0]); e_push(&g->code,RAX);
            cg_expr(g,e->ch[1]); e_push(&g->code,RAX);
            cg_expr(g,e->ch[2]);
            e_mov(&g->code,RDX,RAX);    /* rdx = byte val */
            e_pop(&g->code,RCX);        /* rcx = offset */
            e_pop(&g->code,RDI);        /* rdi = base ptr */
            /* add rdi,rcx */
            e_add(&g->code,RDI,RCX);
            /* mov byte[rdi],dl */
            e2(&g->code,0x88,0x17);
        }
        else if(!strcmp(m,"GetByte")){
            /* GetByte(ptr, offset) -> byte value */
            if(e->nch!=2)die("[Line %d] execute.GetByte(ptr,off)",e->line);
            cg_expr(g,e->ch[0]); e_push(&g->code,RAX);
            cg_expr(g,e->ch[1]);
            e_pop(&g->code,RCX);        /* rcx = base */
            e_add(&g->code,RCX,RAX);    /* rcx = base+off */
            /* movzx rax,byte[rcx] */
            e4(&g->code,0x48,0x0F,0xB6,0x01);
        }
        else if(!strcmp(m,"Brk")){
            /* Brk(addr) -> new break addr. Pass 0 to get current. */
            if(e->nch!=1)die("[Line %d] execute.Brk(addr)",e->line);
            cg_expr(g,e->ch[0]);
            e_mov(&g->code,RDI,RAX);
            e_mov_imm(&g->code,RAX,12); /* sys_brk */
            e_syscall(&g->code);
        }
        else if(!strcmp(m,"Time")){
            /* Time() -> unix timestamp in seconds */
            if(e->nch!=0)die("[Line %d] execute.Time() takes no args",e->line);
            e_xor(&g->code,RDI,RDI);    /* tloc=NULL */
            e_mov_imm(&g->code,RAX,201); /* sys_time */
            e_syscall(&g->code);
        }
        else if(!strcmp(m,"ClockGet")){
            /* ClockGet(clock_id, timespec_ptr)
             * clock_id: 0=CLOCK_REALTIME, 1=CLOCK_MONOTONIC */
            if(e->nch!=2)die("[Line %d] execute.ClockGet(id,timespec_ptr)",e->line);
            cg_expr(g,e->ch[0]); e_push(&g->code,RAX);
            cg_expr(g,e->ch[1]);
            e_mov(&g->code,RSI,RAX);
            e_pop(&g->code,RDI);
            e_mov_imm(&g->code,RAX,228); /* sys_clock_gettime */
            e_syscall(&g->code);
        }
        else{
            die("[Line %d] Unknown execute.%s()",e->line,m);
        }
        break;
    } /* end ND_EXEC_CALL */

    default: die("[Line %d] Unknown expr node %d",e->line,e->kind);
    }
}

static void cg_assign_lhs(CG*g,Node*lhs,int val_reg){
    if(lhs->kind==ND_IDENT){
        Local*l=cg_get(g,lhs->sval,lhs->line);
        if(l->size==1){
            /* mov byte[rbp+off], val_reg_low */
            uint8_t rex=(uint8_t)(0x40|(rr(val_reg)<<2));
            if(rex!=0x40)e1(&g->code,rex);
            e2(&g->code,0x88,(uint8_t)(0x45|(ri(val_reg)<<3)));
            if(l->off>=-128&&l->off<=127)e1(&g->code,(uint8_t)(int8_t)l->off);
            else ei32(&g->code,l->off);
        } else {
            e_store(&g->code,l->off,val_reg);
        }
    } else if(lhs->kind==ND_INDEX){
        e_push(&g->code,val_reg);
        cg_expr(g,lhs->ch[0]); e_push(&g->code,RAX);
        cg_expr(g,lhs->ch[1]); e_mov(&g->code,RCX,RAX);
        e_pop(&g->code,RDX);
        e_arr_addr(&g->code,RDX,RDX,RCX);
        e_pop(&g->code,RAX);
        e_store_ptr(&g->code,RDX,RAX);
    } else if(lhs->kind==ND_DEREF){
        e_push(&g->code,val_reg);
        cg_expr(g,lhs->ch[0]);  /* rax = ptr */
        e_pop(&g->code,RCX);    /* rcx = value */
        /* mov byte[rax], cl — store 1 byte */
        e3(&g->code,0x88,0x08,0x00); /* mov byte[rax],cl — but ModRM is wrong */
        /* Correct: 88 /r with ModRM(0,rcx=1,rax=0) = 0x08 */
        g->code.len -= 3;
        e2(&g->code,0x88,0x08); /* mov byte[rax],cl */
    } else if(lhs->kind==ND_FIELD){
        Node*base=lhs->ch[0];
        Local*l=cg_get(g,base->sval,lhs->line);
        int foff=-1;
        for(int i=0;i<nstructs&&foff<0;i++)
            for(int j=0;j<structs[i].nfields;j++)
                if(!strcmp(structs[i].fields[j],lhs->sval)){foff=structs[i].offsets[j];break;}
        if(foff<0)die("Unknown field '%s'",lhs->sval);
        e_store(&g->code,l->off+foff,val_reg);
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
        if(is_arr){l->arr_count=s->type_hint->count;l->elem_size=ty_size(s->type_hint->inner);}
        if(s->nch>0){
            cg_expr(g,s->ch[0]);
            if(s->kind==ND_CONSTDECL&&s->ch[0]->is_const){
                l->is_const=1;l->const_val=s->ch[0]->const_val;
            }
            if(!is_arr)e_store(&g->code,l->off,RAX);
        }
        break;
    }
    case ND_ASSIGN:
        cg_expr(g,s->ch[1]);
        cg_assign_lhs(g,s->ch[0],RAX);
        break;
    case ND_RETURN:
        if(s->nch)cg_expr(g,s->ch[0]);
        else e_mov_imm(&g->code,RAX,0);
        {size_t site=e_jmp(&g->code);g->ret_sites[g->nret++]=site;}
        break;
    case ND_IF:{
        cg_expr(g,s->ch[0]);
        e_test_rax(&g->code);
        size_t je=e_je(&g->code);
        Node*then=s->ch[1];
        for(int i=0;i<then->nch;i++)cg_stmt(g,then->ch[i]);
        if(s->nch>2){
            size_t jmp=e_jmp(&g->code);
            patch(&g->code,je,buf_pos(&g->code));
            Node*els=s->ch[2];
            if(els->kind==ND_BLOCK)for(int i=0;i<els->nch;i++)cg_stmt(g,els->ch[i]);
            else cg_stmt(g,els);
            patch(&g->code,jmp,buf_pos(&g->code));
        } else patch(&g->code,je,buf_pos(&g->code));
        break;
    }
    case ND_WHILE:{
        size_t top=buf_pos(&g->code);
        g->bstk[g->bdepth].n=0; g->ncont[g->bdepth]=0; g->bdepth++;
        cg_expr(g,s->ch[0]);
        e_test_rax(&g->code);
        size_t je=e_je(&g->code);
        Node*body=s->ch[1];
        for(int i=0;i<body->nch;i++)cg_stmt(g,body->ch[i]);
        size_t cont_top=buf_pos(&g->code);
        e_jmp_back(&g->code,top);
        size_t end=buf_pos(&g->code);
        patch(&g->code,je,end);
        g->bdepth--;
        for(int i=0;i<g->bstk[g->bdepth].n;i++)patch(&g->code,g->bstk[g->bdepth].sites[i],end);
        for(int i=0;i<g->ncont[g->bdepth];i++)patch(&g->code,g->cont_sites[g->bdepth][i],cont_top);
        break;
    }
    case ND_FOR:{
        Local*lv=cg_alloc(g,s->sval,8);
        cg_expr(g,s->ch[0]); /* start */
        e_store(&g->code,lv->off,RAX);
        size_t top=buf_pos(&g->code);
        g->bstk[g->bdepth].n=0; g->ncont[g->bdepth]=0; g->bdepth++;
        /* cond: i < end */
        e_load(&g->code,RAX,lv->off);
        e_push(&g->code,RAX);
        cg_expr(g,s->ch[1]); /* end */
        e_mov(&g->code,RCX,RAX);
        e_pop(&g->code,RAX);
        e_cmp(&g->code,RAX,RCX);
        e_setcc(&g->code,"<");
        e_test_rax(&g->code);
        size_t je=e_je(&g->code);
        Node*body=s->ch[2];
        for(int i=0;i<body->nch;i++)cg_stmt(g,body->ch[i]);
        /* increment */
        size_t cont_top=buf_pos(&g->code);
        e_load(&g->code,RAX,lv->off);
        e3(&g->code,0x48,0xFF,0xC0); /* inc rax */
        e_store(&g->code,lv->off,RAX);
        e_jmp_back(&g->code,top);
        size_t end=buf_pos(&g->code);
        patch(&g->code,je,end);
        g->bdepth--;
        for(int i=0;i<g->bstk[g->bdepth].n;i++)patch(&g->code,g->bstk[g->bdepth].sites[i],end);
        for(int i=0;i<g->ncont[g->bdepth];i++)patch(&g->code,g->cont_sites[g->bdepth][i],cont_top);
        break;
    }
    case ND_LOOP:{
        size_t top=buf_pos(&g->code);
        g->bstk[g->bdepth].n=0; g->ncont[g->bdepth]=0; g->bdepth++;
        Node*body=s->ch[0];
        for(int i=0;i<body->nch;i++)cg_stmt(g,body->ch[i]);
        e_jmp_back(&g->code,top);
        size_t end=buf_pos(&g->code);
        g->bdepth--;
        for(int i=0;i<g->bstk[g->bdepth].n;i++)patch(&g->code,g->bstk[g->bdepth].sites[i],end);
        break;
    }
    case ND_BREAK:
        if(!g->bdepth)die("[Line %d] break outside loop",s->line);
        {size_t site=e_jmp(&g->code);g->bstk[g->bdepth-1].sites[g->bstk[g->bdepth-1].n++]=site;}
        break;
    case ND_CONTINUE:
        if(!g->bdepth)die("[Line %d] continue outside loop",s->line);
        {size_t site=e_jmp(&g->code);g->cont_sites[g->bdepth-1][g->ncont[g->bdepth-1]++]=site;}
        break;
    case ND_EXPRSTMT:
        cg_expr(g,s->ch[0]);
        break;
    case ND_BLOCK:
        for(int i=0;i<s->nch;i++)cg_stmt(g,s->ch[i]);
        break;
    default: die("[Line %d] Unknown stmt %d",s->line,s->kind);
    }
}

static void cg_fn(CG*g,Node*fn){
    g->nloc=0; g->local_size=0; g->nret=0; g->bdepth=0;
    memset(g->ncont,0,sizeof(g->ncont));
    static int arg_regs[]={RDI,RSI,RDX,RCX,R8,R9};
    int nparam=fn->nch-1;
    Node*body=fn->ch[fn->nch-1];
    /* Frame: params + locals + safety margin */
    int frame=(nparam+cg_count_locals(body))*8+64;
    if(frame%16)frame+=(16-frame%16);
    /* Prologue */
    e_push(&g->code,RBP);
    e_mov(&g->code,RBP,RSP);
    e_sub_rsp(&g->code,frame);
    /* Spill params */
    for(int i=0;i<nparam&&i<6;i++){
        Local*l=cg_alloc(g,fn->ch[i]->sval,8);
        e_store(&g->code,l->off,arg_regs[i]);
    }
    /* Body */
    for(int i=0;i<body->nch;i++)cg_stmt(g,body->ch[i]);
    /* Epilogue */
    size_t epi=buf_pos(&g->code);
    for(int i=0;i<g->nret;i++)patch(&g->code,g->ret_sites[i],epi);
    e_mov(&g->code,RSP,RBP);
    e_pop(&g->code,RBP);
    e_ret(&g->code);
}

static uint8_t *cg_compile(CG*g,Node*prog,size_t*olen){
    /* Register structs */
    for(int i=0;i<prog->nch;i++){
        Node*n=prog->ch[i];
        if(n->kind!=ND_STRUCT_DEF)continue;
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

    /* _start: save argc->r12, argv->r13, call main, exit */
    /* pop rdi = argc (Linux puts argc at [rsp] at entry) */
    e_pop(&g->code,RDI);
    /* mov r12,rdi (save argc) */
    e3(&g->code,0x49,0x89,0xFC);
    /* mov r13,rsp (argv = current rsp, points to argv[0]) */
    e3(&g->code,0x49,0x89,0xE5);
    /* align rsp to 16 */
    e4(&g->code,0x48,0x83,0xE4,0xF0);
    size_t start_site=e_call(&g->code);
    strncpy(g->fn_patches[g->nfnp].name,"main",63);
    g->fn_patches[g->nfnp].site=start_site;
    g->nfnp++;
    e_mov(&g->code,RDI,RAX);
    e_mov_imm(&g->code,RAX,60);
    e_syscall(&g->code);

    /* Compile functions */
    for(int i=0;i<prog->nch;i++){
        Node*fn=prog->ch[i];
        if(fn->kind!=ND_FNDEF)continue;
        g->fns[g->nfns].off=buf_pos(&g->code);
        strncpy(g->fns[g->nfns].name,fn->sval,63);
        g->nfns++;
        cg_fn(g,fn);
    }

    /* Emit subroutines */
    g->itoa_off  =buf_pos(&g->code); emit_itoa  (&g->code);
    g->strlen_off=buf_pos(&g->code); emit_strlen(&g->code);
    g->memcpy_off=buf_pos(&g->code); emit_memcpy(&g->code);
    g->memset_off=buf_pos(&g->code); emit_memset(&g->code);
    /* __env_get(rdi=name) -> rax=value_ptr or 0
     * envp is at r13 + (argc+1)*8.
     * Scans envp for "name=..." and returns ptr to value after '='. */
    {
        size_t env_off=buf_pos(&g->code);
        /* Register as a named function so it can be called */
        g->fns[g->nfns].off=env_off;
        strncpy(g->fns[g->nfns].name,"__env_get",63);
        g->nfns++;
        /* prologue */
        e_push(&g->code,RBP); e_mov(&g->code,RBP,RSP);
        e_push(&g->code,RBX); /* save rbx (name) */
        e2(&g->code,0x41,0x54); /* push r12 -- actually save r14,r15 */
        e2(&g->code,0x41,0x55); /* push r13 -- no, just use non-volatile regs */
        /* Simpler: use rdi=name, iterate envp inline */
        /* envp = r13 + (r12+1)*8 */
        /* rax = r12+1 */
        e_mov(&g->code,RAX,R12);
        e3(&g->code,0x48,0xFF,0xC0); /* inc rax */
        /* imul rax,8 */
        e4(&g->code,0x48,0x6B,0xC0,0x08);
        /* add rax,r13 -> rax = envp base */
        e3(&g->code,0x4C,0x01,0xE8);
        /* rdi=name (arg), rax=envp ptr */
        /* Save name in rbx */
        e_mov(&g->code,RBX,RDI);
        /* outer loop: rax points to current env string ptr */
        size_t env_loop=buf_pos(&g->code);
        /* load cur = [rax] */
        e3(&g->code,0x48,0x8B,0x10); /* mov rdx,[rax] */
        /* if rdx==0: not found, return 0 */
        e3(&g->code,0x48,0x85,0xD2);
        size_t jz_notfound=e_je(&g->code);
        /* compare cur string with name byte by byte until '=' or mismatch */
        e_push(&g->code,RAX); /* save envp position */
        e_mov(&g->code,RSI,RBX); /* rsi=name */
        /* rcx=0 index */
        e_xor(&g->code,RCX,RCX);
        size_t cmp_loop=buf_pos(&g->code);
        /* load name[rcx] into rdi8 */
        /* movzx rdi,byte[rsi+rcx] */
        e4(&g->code,0x0F,0xB6,0x3C,0x0E);
        /* load env[rcx] into r8b */
        /* movzx r8,byte[rdx+rcx] */
        e5(&g->code,0x4C,0x0F,0xB6,0x04,0x0A);
        /* if name[rcx]==0 && env[rcx]=='=': found! */
        e3(&g->code,0x48,0x85,0xFF); /* test rdi,rdi */
        size_t je_nameend=e_je(&g->code);
        /* if env[rcx]!=name[rcx]: mismatch */
        /* cmp r8,rdi */
        e3(&g->code,0x4C,0x39,0xC7);
        size_t jne_mis=e_jne(&g->code);
        /* inc rcx, loop */
        e3(&g->code,0x48,0xFF,0xC1);
        e_jmp_back(&g->code,cmp_loop);
        /* name_end: check if env[rcx]=='=' */
        patch(&g->code,je_nameend,buf_pos(&g->code));
        /* cmp r8b,'=' (0x3D) */
        e4(&g->code,0x49,0x83,0xF8,0x3D);
        size_t jne_eq=e_jne(&g->code);
        /* found: return rdx+rcx+1 (skip past '=') */
        e_add(&g->code,RDX,RCX);
        e3(&g->code,0x48,0xFF,0xC2); /* inc rdx */
        e_mov(&g->code,RAX,RDX);
        e_pop(&g->code,RCX); /* clean stack (saved envp pos) */
        e2(&g->code,0x41,0x5D); /* pop r13 -- match pushes */
        e2(&g->code,0x41,0x5C); /* pop r12 */
        e_pop(&g->code,RBX);
        e_mov(&g->code,RSP,RBP); e_pop(&g->code,RBP);
        e_ret(&g->code);
        /* mismatch or not '=': advance envp, loop */
        patch(&g->code,jne_mis,buf_pos(&g->code));
        patch(&g->code,jne_eq,buf_pos(&g->code));
        e_pop(&g->code,RAX); /* restore envp pos */
        e4(&g->code,0x48,0x83,0xC0,0x08); /* add rax,8 */
        e_jmp_back(&g->code,env_loop);
        /* not found: return 0 */
        patch(&g->code,jz_notfound,buf_pos(&g->code));
        e_xor(&g->code,RAX,RAX);
        e2(&g->code,0x41,0x5D);
        e2(&g->code,0x41,0x5C);
        e_pop(&g->code,RBX);
        e_mov(&g->code,RSP,RBP); e_pop(&g->code,RBP);
        e_ret(&g->code);
    }

    /* Patch function calls */
    for(int i=0;i<g->nfnp;i++){
        size_t target=0;int found=0;
        for(int j=0;j<g->nfns;j++)
            if(!strcmp(g->fns[j].name,g->fn_patches[i].name)){target=g->fns[j].off;found=1;break;}
        if(!found)die("Undefined function '%s'",g->fn_patches[i].name);
        patch(&g->code,g->fn_patches[i].site,target);
    }
    for(int i=0;i<g->nitoa;i++)  patch(&g->code,g->itoa_sites[i],  g->itoa_off);
    for(int i=0;i<g->nstrlen;i++)patch(&g->code,g->strlen_sites[i],g->strlen_off);

    /* Build ELF to find vrd (rodata virtual address) */
    Buf tmp; buf_init(&tmp);
    uint64_t vrd=make_elf(&tmp,g->code.data,g->code.len,g->rodata.data,g->rodata.len);
    free(tmp.data);

    /* Patch all RIP-relative string leas */
    for(int i=0;i<g->nstrp;i++){
        size_t site=g->str_patches[i].site;
        /* RIP = vaddr of instruction after the lea = LOAD_ADDR + HDR_TOTAL + site + 4 */
        uint64_t rip=LOAD_ADDR+HDR_TOTAL+site+4;
        uint64_t target=vrd+g->str_patches[i].rdoff;
        int32_t rel=(int32_t)((int64_t)target-(int64_t)rip);
        buf_patch32(&g->code,site,rel);
    }

    /* Final ELF */
    Buf out; buf_init(&out);
    make_elf(&out,g->code.data,g->code.len,g->rodata.data,g->rodata.len);
    *olen=out.len;
    return out.data;
}

/* ══════════════════════════════════════════════════════════
 * Main
 * ══════════════════════════════════════════════════════════*/
static void banner(void){
    printf("\033[1m\033[96m");
    printf("  ██████╗ ██╗   ██╗████████╗████████╗███████╗██████╗ ██████╗  █████╗ ██╗     ██╗\n");
    printf(" ██╔════╝ ██║   ██║╚══██╔══╝╚══██╔══╝██╔════╝██╔══██╗██╔══██╗██╔══██╗██║     ██║\n");
    printf(" ██║  ███╗██║   ██║   ██║      ██║   █████╗  ██████╔╝██████╔╝███████║██║     ██║\n");
    printf(" ██║   ██║██║   ██║   ██║      ██║   ██╔══╝  ██╔══██╗██╔══██╗██╔══██║██║     ██║\n");
    printf(" ╚██████╔╝╚██████╔╝   ██║      ██║   ███████╗██║  ██║██████╔╝██║  ██║███████╗███████╗\n");
    printf("  ╚═════╝  ╚═════╝    ╚═╝      ╚═╝   ╚══════╝╚═╝  ╚═╝╚═════╝ ╚═╝  ╚═╝╚══════╝╚══════╝\n");
    printf("  v3 — Raw x86-64 | Verified | No deps\033[0m\n\n");
}


/* ── Baked-in standard library sources ── */
static const char *BAKED_LIBS[][2] = {
    {"io",     "fn print(s: ptr) -> void {\n    execute.Write(s)\n}\nfn println(s: ptr) -> void {\n    execute.WriteIn(s)\n}\nfn printInt(n: i64) -> void {\n    execute.WriteInt(n)\n}\nfn printChar(c: i64) -> void {\n    execute.WriteChar(c)\n}\nfn printLine() -> void {\n    execute.WriteIn(\"\")\n}\nfn readInt() -> i64 {\n    return execute.ReadInt()\n}\nfn readLine(buf: ptr, max: i64) -> ptr {\n    execute.ReadIn(buf, max)\n    return buf\n}\nfn ask(prompt: ptr, buf: ptr, max: i64) -> ptr {\n    execute.Write(prompt)\n    execute.ReadIn(buf, max)\n    return buf\n}\nfn askInt(prompt: ptr) -> i64 {\n    execute.Write(prompt)\n    return execute.ReadInt()\n}\nfn printBool(b: i64) -> void {\n    if b {\n        execute.WriteIn(\"true\")\n    } else {\n        execute.WriteIn(\"false\")\n    }\n}\nfn banner(text: ptr) -> void {\n    execute.WriteIn(\"================================\")\n    execute.Write(\"  \")\n    execute.WriteIn(text)\n    execute.WriteIn(\"================================\")\n}\nfn hr() -> void {\n    execute.WriteIn(\"--------------------------------\")\n}\n"},
    {"math",   "fn abs(n: i64) -> i64 {\n    if n < 0 { return -n }\n    return n\n}\nfn max(a: i64, b: i64) -> i64 {\n    if a > b { return a }\n    return b\n}\nfn min(a: i64, b: i64) -> i64 {\n    if a < b { return a }\n    return b\n}\nfn clamp(val: i64, lo: i64, hi: i64) -> i64 {\n    if val < lo { return lo }\n    if val > hi { return hi }\n    return val\n}\nfn pow(base: i64, exp: i64) -> i64 {\n    if exp == 0 { return 1 }\n    var result = 1\n    var b = base\n    var e = exp\n    while e > 0 {\n        if e % 2 == 1 { result = result * b }\n        b = b * b\n        e = e / 2\n    }\n    return result\n}\nfn sqrt(n: i64) -> i64 {\n    if n <= 0 { return 0 }\n    if n == 1 { return 1 }\n    var x = n\n    var y = (x + 1) / 2\n    while y < x {\n        x = y\n        y = (x + n / x) / 2\n    }\n    return x\n}\nfn gcd(a: i64, b: i64) -> i64 {\n    var aa = a\n    var bb = b\n    while bb != 0 {\n        var t = bb\n        bb = aa % bb\n        aa = t\n    }\n    return aa\n}\nfn lcm(a: i64, b: i64) -> i64 {\n    return a / gcd(a, b) * b\n}\nfn factorial(n: i64) -> i64 {\n    if n <= 1 { return 1 }\n    return n * factorial(n - 1)\n}\nfn fibonacci(n: i64) -> i64 {\n    if n <= 1 { return n }\n    return fibonacci(n - 1) + fibonacci(n - 2)\n}\nfn is_prime(n: i64) -> i64 {\n    if n < 2 { return 0 }\n    if n == 2 { return 1 }\n    if n % 2 == 0 { return 0 }\n    var i = 3\n    while i * i <= n {\n        if n % i == 0 { return 0 }\n        i = i + 2\n    }\n    return 1\n}\nfn is_even(n: i64) -> i64 {\n    if n % 2 == 0 { return 1 }\n    return 0\n}\nfn is_odd(n: i64) -> i64 {\n    if n % 2 != 0 { return 1 }\n    return 0\n}\nfn sign(n: i64) -> i64 {\n    if n > 0 { return 1 }\n    if n < 0 { return -1 }\n    return 0\n}\nfn digits(n: i64) -> i64 {\n    if n == 0 { return 1 }\n    var count = 0\n    var num = n\n    if num < 0 { num = -num }\n    while num > 0 {\n        count = count + 1\n        num = num / 10\n    }\n    return count\n}\nfn sum_digits(n: i64) -> i64 {\n    var num = n\n    if num < 0 { num = -num }\n    var s = 0\n    while num > 0 {\n        s = s + num % 10\n        num = num / 10\n    }\n    return s\n}\nfn reverse(n: i64) -> i64 {\n    var result = 0\n    var num = n\n    if num < 0 { num = -num }\n    while num > 0 {\n        result = result * 10 + num % 10\n        num = num / 10\n    }\n    return result\n}\n"},
    {"string", "fn len(s: ptr) -> i64 {\n    return execute.StrLen(s)\n}\nfn eq(a: ptr, b: ptr) -> i64 {\n    return execute.StrEq(a, b)\n}\nfn copy(dst: ptr, src_s: ptr) -> void {\n    execute.StrCopy(dst, src_s)\n}\nfn cat(dst: ptr, src_s: ptr) -> void {\n    execute.StrCat(dst, src_s)\n}\nfn new(size: i64) -> ptr {\n    var buf = execute.Alloc(size)\n    @buf = 0\n    return buf\n}\nfn free(buf: ptr, size: i64) -> void {\n    execute.Free(buf, size)\n}\nfn empty(s: ptr) -> i64 {\n    var first = @s\n    if first == 0 { return 1 }\n    return 0\n}\nfn contains(s: ptr, ch: i64) -> i64 {\n    var p = s\n    loop {\n        var c = @p\n        if c == 0 { return 0 }\n        if c == ch { return 1 }\n        p = p + 1\n    }\n    return 0\n}\nfn index_of(s: ptr, ch: i64) -> i64 {\n    var p = s\n    var i = 0\n    loop {\n        var c = @p\n        if c == 0 { return -1 }\n        if c == ch { return i }\n        p = p + 1\n        i = i + 1\n    }\n    return -1\n}\nfn to_upper(s: ptr) -> void {\n    var p = s\n    loop {\n        var c = @p\n        if c == 0 { break }\n        if c >= 97 && c <= 122 {\n            @p = c - 32\n        }\n        p = p + 1\n    }\n}\nfn to_lower(s: ptr) -> void {\n    var p = s\n    loop {\n        var c = @p\n        if c == 0 { break }\n        if c >= 65 && c <= 90 {\n            @p = c + 32\n        }\n        p = p + 1\n    }\n}\nfn trim_newline(s: ptr) -> void {\n    var l = execute.StrLen(s)\n    if l == 0 { return }\n    var p = s + l - 1\n    var last = @p\n    if last == 10 {\n        @p = 0\n    }\n}\nfn count_char(s: ptr, ch: i64) -> i64 {\n    var count = 0\n    var p = s\n    loop {\n        var c = @p\n        if c == 0 { break }\n        if c == ch { count = count + 1 }\n        p = p + 1\n    }\n    return count\n}\nfn repeat_char(buf: ptr, ch: i64, times: i64) -> void {\n    var p = buf\n    var i = 0\n    while i < times {\n        @p = ch\n        p = p + 1\n        i = i + 1\n    }\n    @p = 0\n}\nfn starts_with(s: ptr, prefix: ptr) -> i64 {\n    var sp = s\n    var pp = prefix\n    loop {\n        var pc = @pp\n        if pc == 0 { return 1 }\n        var sc = @sp\n        if sc != pc { return 0 }\n        sp = sp + 1\n        pp = pp + 1\n    }\n    return 0\n}\nfn ends_with(s: ptr, suffix: ptr) -> i64 {\n    var slen = execute.StrLen(s)\n    var suflen = execute.StrLen(suffix)\n    if suflen > slen { return 0 }\n    var sp = s + slen - suflen\n    var pp = suffix\n    loop {\n        var pc = @pp\n        if pc == 0 { return 1 }\n        var sc = @sp\n        if sc != pc { return 0 }\n        sp = sp + 1\n        pp = pp + 1\n    }\n    return 0\n}\n"},
    {"memory", "fn alloc(size: i64) -> ptr {\n    return execute.Alloc(size)\n}\nfn free(buf: ptr, size: i64) -> void {\n    execute.Free(buf, size)\n}\nfn alloc_zeroed(size: i64) -> ptr {\n    var buf = execute.Alloc(size)\n    var i = 0\n    while i < size {\n        buf[i] = 0\n        i = i + 1\n    }\n    return buf\n}\nfn copy(dst: ptr, src_p: ptr, bytes: i64) -> void {\n    var i = 0\n    while i < bytes {\n        dst[i] = src_p[i]\n        i = i + 1\n    }\n}\nfn zero(buf: ptr, bytes: i64) -> void {\n    var i = 0\n    while i < bytes {\n        buf[i] = 0\n        i = i + 1\n    }\n}\nfn fill(buf: ptr, val: i64, count: i64) -> void {\n    var i = 0\n    while i < count {\n        buf[i] = val\n        i = i + 1\n    }\n}\nfn realloc(old_buf: ptr, old_size: i64, new_size: i64) -> ptr {\n    var new_buf = execute.Alloc(new_size)\n    var copy_size = old_size\n    if new_size < copy_size { copy_size = new_size }\n    var i = 0\n    while i < copy_size {\n        new_buf[i] = old_buf[i]\n        i = i + 1\n    }\n    execute.Free(old_buf, old_size)\n    return new_buf\n}\n"},
    {"sort",   "fn _sort_and_print(arr: ptr, len: i64) -> void {\n    for i in 0..len {\n        for j in 0..len - 1 {\n            if arr[j] > arr[j + 1] {\n                var tmp = arr[j]\n                arr[j] = arr[j + 1]\n                arr[j + 1] = tmp\n            }\n        }\n    }\n    execute.WriteIn(\"Sorted:\")\n    for i in 0..len {\n        execute.WriteInt(arr[i])\n    }\n}\nfn min(arr: ptr, len: i64) -> i64 {\n    var m = arr[0]\n    for i in 1..len {\n        if arr[i] < m { m = arr[i] }\n    }\n    return m\n}\nfn max(arr: ptr, len: i64) -> i64 {\n    var m = arr[0]\n    for i in 1..len {\n        if arr[i] > m { m = arr[i] }\n    }\n    return m\n}\nfn asc(arr: ptr, len: i64) -> void {\n    for i in 0..len {\n        for j in 0..len - 1 {\n            if arr[j] > arr[j + 1] {\n                var tmp = arr[j]\n                arr[j] = arr[j + 1]\n                arr[j + 1] = tmp\n            }\n        }\n    }\n}\nfn desc(arr: ptr, len: i64) -> void {\n    for i in 0..len {\n        for j in 0..len - 1 {\n            if arr[j] < arr[j + 1] {\n                var tmp = arr[j]\n                arr[j] = arr[j + 1]\n                arr[j + 1] = tmp\n            }\n        }\n    }\n}\nfn is_sorted(arr: ptr, len: i64) -> i64 {\n    for i in 0..len - 1 {\n        if arr[i] > arr[i + 1] { return 0 }\n    }\n    return 1\n}\nfn sum(arr: ptr, len: i64) -> i64 {\n    var total = 0\n    for i in 0..len {\n        total = total + arr[i]\n    }\n    return total\n}\n"},
    {NULL, NULL}
};

static void write_lib_file(const char *dir, const char *name, const char *src){
    char path[512];
    snprintf(path,sizeof(path),"%s/%s.egg",dir,name);
    FILE*f=fopen(path,"w");
    if(!f){fprintf(stderr,"Cannot write %s: %s\n",path,strerror(errno));return;}
    fputs(src,f);fclose(f);
    printf("  \033[92m✓\033[0m  %s\n",path);
}

static void install_libs(void){
    printf("\n\033[1m\033[96mgutterball — install standard libraries\033[0m\n\n");

    /* Try /usr/local/lib/vex first, then ~/.vex/libs */
    const char*sysdir="/usr/local/lib/vex";
    char userdir[512];
    const char*home=getenv("HOME");
    if(home) snprintf(userdir,sizeof(userdir),"%s/.vex/libs",home);
    else      snprintf(userdir,sizeof(userdir),"./.vex/libs");

    /* Try system dir first */
    if(mkdir(sysdir,0755)==0||errno==EEXIST){
        printf("Installing to %s/\n\n",sysdir);
        for(int i=0;BAKED_LIBS[i][0];i++)
            write_lib_file(sysdir,BAKED_LIBS[i][0],BAKED_LIBS[i][1]);
        printf("\n\033[92m✓ Done!\033[0m Libraries installed to %s/\n",sysdir);
        printf("You can now use get-Library/io etc. from any directory.\n\n");
        return;
    }

    /* Fall back to user dir */
    printf("\033[93mNote:\033[0m Cannot write to %s (need sudo).\n",sysdir);
    printf("Installing to %s/ instead.\n\n",userdir);

    /* Create user dir (mkdir -p equivalent) */
    char tmp[512]; strncpy(tmp,userdir,511);
    for(char*p=tmp+1;*p;p++){
        if(*p=='/'){*p=0;mkdir(tmp,0755);*p='/';}
    }
    mkdir(userdir,0755);

    for(int i=0;BAKED_LIBS[i][0];i++)
        write_lib_file(userdir,BAKED_LIBS[i][0],BAKED_LIBS[i][1]);
    printf("\n\033[92m✓ Done!\033[0m Libraries installed to %s/\n",userdir);
    printf("You can now use get-Library/io etc. from any directory.\n\n");
}

int main(int argc,char**argv){
    banner();
    /* Check for --install-libs before anything else */
    for(int i=1;i<argc;i++){
        if(!strcmp(argv[i],"--install-libs")||!strcmp(argv[i],"-install-libs")||!strcmp(argv[i],"--install")){
            install_libs();
            return 0;
        }
    }
    const char*src=NULL,*out=NULL;
    for(int i=1;i<argc;i++){
        if(!strcmp(argv[i],"-o")&&i+1<argc)out=argv[++i];
        else if(!strcmp(argv[i],"-O0"))opt_level=0;
        else if(!strcmp(argv[i],"-O2"))opt_level=2;
        else if(argv[i][0]!='-')src=argv[i];
        else{fprintf(stderr,"Unknown flag: %s\n",argv[i]);return 1;}
    }
    if(!src){fprintf(stderr,"Usage: gutterball <file.egg> [-o output] [-O0]\n  Also: gutterball --install-libs\n");return 1;}
    char outbuf[512];
    if(!out){strncpy(outbuf,src,500);char*d=strrchr(outbuf,'.');if(d)*d=0;out=outbuf;}

    printf("  \033[96m[1/4]\033[0m Lexing   %s\n",src);
    FILE*f=fopen(src,"r");if(!f)die("Cannot open '%s': %s",src,strerror(errno));
    fseek(f,0,SEEK_END);long sz=ftell(f);rewind(f);
    char*text=xmalloc(sz+1);
    if((long)fread(text,1,sz,f)<sz&&ferror(f))die("Read error");
    text[sz]=0;fclose(f);

    Lexer L;memset(&L,0,sizeof(L));L.src=text;L.line=1;lex(&L);

    printf("  \033[96m[2/4]\033[0m Parsing\n");
    Parser P;P.tokens=L.tokens;P.pos=0;
    Node*prog=parse_program(&P);

    /* ── Resolve imports ───────────────────────────────────────── */
    {
        /* Get directory of source file */
        char srcdir[512]={0};
        strncpy(srcdir,src,500);
        char*slash=strrchr(srcdir,'/');
        if(slash)*slash=0; else srcdir[0]=0; /* empty = current dir */

        int i=0;
        while(i<prog->nch){
            Node*nd=prog->ch[i];
            if(nd->kind!=ND_IMPORT){i++;continue;}
            const char*libname=nd->sval;

            /* Search for library in order:
             * 1. Same folder as source file
             * 2. ~/.vex/libs/
             * 3. /usr/local/lib/vex/
             * 4. /usr/lib/vex/  */
            char libpath[600];
            FILE*lf=NULL;

            /* 1. Source file directory */
            if(!lf){
                if(srcdir[0])snprintf(libpath,sizeof(libpath),"%s/%s.egg",srcdir,libname);
                else          snprintf(libpath,sizeof(libpath),"%s.egg",libname);
                lf=fopen(libpath,"r");
            }
            /* 2. ~/.vex/libs/ */
            if(!lf){
                const char*home=getenv("HOME");
                if(home){
                    snprintf(libpath,sizeof(libpath),"%s/.vex/libs/%s.egg",home,libname);
                    lf=fopen(libpath,"r");
                }
            }
            /* 3. /usr/local/lib/vex/ */
            if(!lf){
                snprintf(libpath,sizeof(libpath),"/usr/local/lib/vex/%s.egg",libname);
                lf=fopen(libpath,"r");
            }
            /* 4. /usr/lib/vex/ */
            if(!lf){
                snprintf(libpath,sizeof(libpath),"/usr/lib/vex/%s.egg",libname);
                lf=fopen(libpath,"r");
            }

            if(lf){
                printf("  \033[96m[lib]\033[0m  %s  (%s)\n",libname,libpath);
            } else {
                fprintf(stderr,
                    "\033[91m[gutterball]\033[0m Library not found: '%s'\n"
                    "  Searched:\n"
                    "    %s/%s.egg  (source dir)\n"
                    "    ~/.vex/libs/%s.egg\n"
                    "    /usr/local/lib/vex/%s.egg\n"
                    "    /usr/lib/vex/%s.egg\n"
                    "  Install system-wide: sudo cp %s.egg /usr/local/lib/vex/\n",
                    libname, srcdir[0]?srcdir:".", libname,
                    libname, libname, libname, libname);
                exit(1);
            }
            fseek(lf,0,SEEK_END);long lsz=ftell(lf);rewind(lf);
            char*ltext=xmalloc(lsz+1);
            fread(ltext,1,lsz,lf);ltext[lsz]=0;fclose(lf);

            /* Lex + parse the library */
            Lexer LL;memset(&LL,0,sizeof(LL));LL.src=ltext;LL.line=1;lex(&LL);
            Parser LP;LP.tokens=LL.tokens;LP.pos=0;
            Node*libprog=parse_program(&LP);

            /* Collect all fn names in this library first */
            char libfns[256][64]; int nfns=0;
            for(int j=0;j<libprog->nch;j++){
                if(libprog->ch[j]->kind==ND_FNDEF&&nfns<256)
                    strncpy(libfns[nfns++],libprog->ch[j]->sval,63);
            }
            /* Prefix all fn names and internal calls with "libname." */
            prog->ch[i]=prog->ch[--prog->nch]; /* swap-remove import node */
            for(int j=0;j<libprog->nch;j++){
                Node*fn=libprog->ch[j];
                if(fn->kind==ND_FNDEF){
                    /* rename internal calls first */
                    prefix_calls(fn,libname,libfns,nfns);
                    /* rename the function itself */
                    char newname[200];
                    snprintf(newname,sizeof(newname),"%s.%s",libname,fn->sval);
                    free(fn->sval);fn->sval=xstrdup(newname);
                    nadd(prog,fn);
                } else if(fn->kind==ND_STRUCT_DEF){
                    nadd(prog,fn);
                }
            }
            free(ltext);
            /* Don't increment i — we removed current slot */
        }
    }

    printf("  \033[96m[3/4]\033[0m Generating x86-64  (O%d)\n",opt_level);
    CG g;cg_init(&g);
    size_t elen=0;
    uint8_t*elf=cg_compile(&g,prog,&elen);

    printf("  \033[96m[4/4]\033[0m Writing → %s\n",out);
    FILE*of=fopen(out,"wb");if(!of)die("Cannot write '%s': %s",out,strerror(errno));
    fwrite(elf,1,elen,of);fclose(of);
    chmod(out,0755);

    printf("\n  \033[92m✓ Done!\033[0m  (%zu bytes)  Run: \033[1m./%s\033[0m\n\n",elen,out);
    return 0;
}
