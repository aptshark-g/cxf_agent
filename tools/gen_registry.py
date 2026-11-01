#!/usr/bin/env python3
"""
从 C 源码生成注册期产物：类型描述符与服务注册项。

两类产物共用一次源码扫描，因为它们本来就是同一件事的两半 ——
注册期要的是"把编译期常量登记进 world"，类型与服务的差别只在于查出来的
是一棵类型树还是一张函数指针表。

分工（这是整个工具的设计核心）：
  生成器只决定"有哪些类型、哪些字段、每个字段是什么种类"，
  不计算任何数值。生成的 C 代码里写的是 sizeof / _Alignof / offsetof，
  由编译器在编译时填实际数字。

这样换来三个性质：
  1. 交叉编译正确 —— 数值永远由目标编译器给出，不依赖生成机
  2. 生成物可读 —— 它是普通 C，能直接看、能断点调试
  3. 宏、typedef、条件编译、匿名结构体都由 clang 处理过了，
     生成器不碰这些，也就不会在这些地方算错

输入：源文件 + CXF_TYPE 标记（类型）+ CXF_SERVICE 标记（服务）
输出：xxx.gen.c + xxx.gen.h

服务不需要 clang：注册项的全部信息都在宏参数里，生成器只取符号名，
把值留给编译期。类型需要 clang，因为结构体内部展开只能靠 AST。
"""

import argparse
import json
import os
import re
import subprocess
import sys

# 内置类型的规范拼写 -> (种类, 期望宽度)
# 期望宽度只用于生成 _Static_assert，防止平台与假设不符时静默错标。
BUILTIN = {
    '_Bool':             ('CXF_K_U8',  1),
    'signed char':       ('CXF_K_I8',  1),
    'unsigned char':     ('CXF_K_U8',  1),
    'short':             ('CXF_K_I16', 2),
    'unsigned short':    ('CXF_K_U16', 2),
    'int':               ('CXF_K_I32', 4),
    'unsigned int':      ('CXF_K_U32', 4),
    'long':              ('CXF_K_I64', 8),
    'unsigned long':     ('CXF_K_U64', 8),
    'long long':         ('CXF_K_I64', 8),
    'unsigned long long': ('CXF_K_U64', 8),
    'float':             ('CXF_K_F32', 4),
    'double':            ('CXF_K_F64', 8),
}

# clang 的规范拼写已足够统一，这里只补少数等价写法
NORMALIZE = {
    'signed': 'int', 'signed int': 'int', 'unsigned': 'unsigned int',
    'short int': 'short', 'signed short': 'short', 'signed short int': 'short',
    'unsigned short int': 'unsigned short',
    'long int': 'long', 'signed long': 'long', 'signed long int': 'long',
    'unsigned long int': 'unsigned long',
    'long long int': 'long long', 'signed long long': 'long long',
    'signed long long int': 'long long',
    'unsigned long long int': 'unsigned long long',
}

# 明确拒绝的内置类型及理由。宁可编译期报错，不要静默错标。
REJECT_BUILTIN = {
    'char': 'char 的符号性由平台决定（ARM AAPCS 为 unsigned，其余多数为 signed），'
            '请显式写 signed char 或 unsigned char',
    'long double': '没有对应的 cxf_kind',
}


def die(msg):
    sys.stderr.write('gen_types: %s\n' % msg)
    sys.exit(1)


def mangle(s):
    """把类型拼写变成 C 标识符后缀。'void *' -> 'void_ptr'。"""
    s = s.replace('struct ', '').replace('union ', '').replace('*', '_ptr')
    s = re.sub(r'[^A-Za-z0-9]+', '_', s).strip('_')
    return s


def strip_comments(text):
    text = re.sub(r'/\*.*?\*/', '', text, flags=re.S)
    text = re.sub(r'//[^\n]*', '', text)
    return text


def scan_markers(path):
    """取 CXF_TYPE(X) 里的名字。文本扫描足够 —— 标记是我们自己的宏。"""
    with open(path, 'r', encoding='utf-8') as f:
        text = strip_comments(f.read())
    return re.findall(r'\bCXF_TYPE\s*\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*\)\s*;', text)


def scan_service_markers(path):
    """
    取 CXF_SERVICE(SYM, ...) 里的符号名。

    服务不像类型那样需要 clang：注册项的值（name / version / api）
    全在宏参数里，而且宏自己会展开成一个真常量。生成器只负责把符号名
    收集成一张 bundle，连值都不必知道 —— 那会让生成物与宏参数脱节。
    """
    with open(path, 'r', encoding='utf-8') as f:
        text = strip_comments(f.read())
    return re.findall(r'\bCXF_SERVICE\s*\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*,', text)


def dump_ast(path, includes, std):
    cmd = ['clang', '-Xclang', '-ast-dump=json', '-fsyntax-only',
           '-x', 'c', '-std=' + std]
    for inc in includes:
        cmd += ['-I', inc]
    cmd.append(path)
    p = subprocess.run(cmd, capture_output=True, text=True)
    if p.returncode != 0:
        sys.stderr.write(p.stderr)
        die('clang 无法解析 %s' % path)
    return json.loads(p.stdout)


def walk(node):
    if isinstance(node, dict):
        yield node
        for v in node.values():
            yield from walk(v)
    elif isinstance(node, list):
        for v in node:
            yield from walk(v)


class Index:
    """从 AST 里建两张名字索引：struct tag 名、typedef 名。"""

    def __init__(self, root):
        self.root = root
        self.by_id = {}
        for n in walk(root):
            if isinstance(n, dict) and n.get('id'):
                self.by_id.setdefault(n['id'], n)

        self.records = [n for n in walk(root)
                        if n.get('kind') == 'RecordDecl'
                        and n.get('completeDefinition')]
        self.by_tag = {}
        for r in self.records:
            if r.get('name'):
                self.by_tag[r['name']] = r

        # 匿名结构体（typedef struct { ... } Vec3;）在 AST 里定义节点与 typedef
        # 是兄弟关系，typedef 子树里只有引用。引用带 ownedTagDecl.id，
        # 用 id 反查才是定义。按名字猜会失败。
        self.by_typedef = {}
        self.typedef_of_record = {}
        for n in walk(root):
            if n.get('kind') != 'TypedefDecl' or not n.get('name'):
                continue
            rec = self.record_of_typedef(n)
            if rec is not None:
                self.by_typedef.setdefault(n['name'], rec)
                self.typedef_of_record.setdefault(rec['id'], n['name'])

    def record_of_typedef(self, node):
        for c in walk(node):
            for ref in (c.get('ownedTagDecl'), c.get('decl')):
                if ref and ref.get('kind') == 'RecordDecl':
                    r = self.by_id.get(ref['id'])
                    if r is not None and r.get('completeDefinition'):
                        return r
        return None

    def find(self, name):
        return self.by_tag.get(name) or self.by_typedef.get(name)

    def cname(self, record):
        """能在 C 里写出来的类型名，供 sizeof / offsetof 用。"""
        alias = self.typedef_of_record.get(record['id'])
        if alias:
            return alias
        if record.get('name'):
            return 'struct ' + record['name']
        return None


def field_type(node):
    """取字段类型的规范拼写。优先脱糖后的写法。"""
    t = node.get('type', {})
    return t.get('desugaredQualType') or t.get('qualType') or ''


class Gen:
    def __init__(self, index):
        self.index = index
        self.builtins = {}   # 规范拼写 -> 变量名
        self.ptrs = {}       # 展示名 -> (变量名, 目标变量名或 None)
        self.structs = {}    # 类型名 -> {'cname','fields','record'}

    # ---- 引用构造：返回 C 里可用的描述符变量名 ----

    def ref_builtin(self, canon):
        canon = NORMALIZE.get(canon, canon)
        if canon in REJECT_BUILTIN:
            die('类型 %s 被拒绝：%s' % (canon, REJECT_BUILTIN[canon]))
        if canon not in BUILTIN:
            die('未知类型 %s：没有对应的 cxf_kind，把它包成结构体或换一种写法' % canon)
        if canon not in self.builtins:
            self.builtins[canon] = 'cxf_bt_' + mangle(canon)
        return self.builtins[canon]

    def ref_ptr(self, pointee, target_var, display):
        """pointee 是能在 C 里写出的被指类型，target_var 是目标描述符或 None。"""
        if pointee not in self.ptrs:
            self.ptrs[pointee] = {
                'var': 'cxf_pt_' + mangle(display),
                'target': target_var,
                'display': display,
            }
        return self.ptrs[pointee]['var']

    # ---- 字段类型解析 ----

    def resolve(self, fnode, owner):
        fname = fnode.get('name', '?')
        t = fnode.get('type', {})
        raw = t.get('qualType', '')
        canon = field_type(fnode)

        def bad(why):
            die('%s.%s：%s（类型 %s）' % (owner, fname, why, raw))

        if '[' in raw:
            bad('数组不支持 —— cxf_kind 里没有数组这一档，加它要先决定'
                '元素是叶子还是继续展开，现阶段拒绝而不是猜')
        if '(' in raw:
            bad('函数指针不支持 —— 它是代码地址，不是数据，没有可展平的叶子')
        if fnode.get('isBitfield'):
            bad('位域不支持 —— offsetof 对位域未定义')

        # 指针：叶子，但要记下目标类型（仅用于诊断，不追踪指向的内容）
        if canon.endswith('*') or raw.endswith('*'):
            base = canon.rstrip('*').strip()
            base = re.sub(r'\bstruct\s+', '', base).strip()
            if base == 'void':
                return self.ref_ptr('void', None, 'void*')
            rec = self.index.find(base)
            if rec is None:
                # 目标没生成描述符，退化成不带 target 的指针，不报错
                return self.ref_ptr(base, None, base + '*')
            self.require(base)
            return self.ref_ptr(self.structs[base]['cname'],
                                '&cxf_type_' + self.structs[base]['cname_key'],
                                (rec.get('name') or base) + '*')

        if canon.startswith('union '):
            bad('union 不支持 —— 大小可知，活跃成员不可知')
        if canon.startswith('struct '):
            tag = canon[len('struct '):].strip()
            rec = self.index.find(tag)
            if rec is None:
                bad('找不到结构体 %s 的定义' % tag)
            self.require(tag)
            return 'cxf_type_' + self.structs[tag]['cname_key']

        # 没有脱糖信息时（匿名 typedef 出来的结构体），顺着 typedef 找到定义
        td = self.index.by_id.get(t.get('typeAliasDeclId'))
        if td is not None:
            rec = self.index.record_of_typedef(td)
            if rec is not None:
                name = td['name']
                self.require(name)
                return 'cxf_type_' + self.structs[name]['cname_key']

        return self.ref_builtin(canon)

    # ---- 结构体：按需展开，依赖自动带出来 ----

    def require(self, name):
        if name in self.structs:
            return
        rec = self.index.find(name)
        if rec is None:
            die('CXF_TYPE(%s) 找不到对应的结构体定义' % name)
        if rec.get('tagUsed') == 'union':
            die('%s 是 union：大小可知，活跃成员不可知' % name)
        cname = self.index.cname(rec)
        if cname is None:
            die('%s 是匿名结构体且没有 typedef 名，无法生成 offsetof' % name)

        key = mangle(name)
        self.structs[name] = {'cname': cname, 'cname_key': key,
                              'record': rec, 'fields': None}

        fields = []
        for c in rec.get('inner', []):
            if c.get('kind') != 'FieldDecl':
                continue
            fields.append((c.get('name'), self.resolve(c, name)))
        if not fields:
            die('%s 没有字段：空结构体在 C 里不合法，也没有可展平的叶子' % name)
        self.structs[name]['fields'] = fields


def emit(gen, svc_syms, src_path, out_c, out_h):
    guard = re.sub(r'[^A-Za-z0-9]+', '_', os.path.basename(out_h)).upper()
    inc = os.path.basename(src_path)
    stem = mangle(re.sub(r'\.(h|c)$', '', inc))
    names = list(gen.structs.keys()) if gen is not None else []

    L = []
    L.append('/* 由 tools/gen_registry.py 生成，勿手改。源：%s */' % inc)
    L.append('')
    if names:
        L.append('#include "%s"' % inc)
        L.append('#include "cxf/type.h"')
    L.append('#include "cxf/service.h"')
    L.append('')

    if names:
        emit_types(gen, names, stem, inc, L)

    if svc_syms:
        # 服务注册项：符号本身由 CXF_SERVICE 在实现的 TU 里定义，
        # 这里只 extern 引用。生成器不知道也不需要知道 name / version / api，
        # 那些值留在宏参数里，编译期才展开。
        L.append('/* 服务注册项：符号由 CXF_SERVICE 定义，此处只引用。 */')
        for s in svc_syms:
            L.append('extern const cxf_service_def cxf_svc_%s;' % s)
        L.append('')
        L.append('static const cxf_service_def *const cxf_svc_entries_%s[] = {' % stem)
        for s in svc_syms:
            L.append('    &cxf_svc_%s,' % s)
        L.append('};')
        L.append('const cxf_service_bundle cxf_services_%s = '
                 '{ cxf_svc_entries_%s, %d, "%s" };'
                 % (stem, stem, len(svc_syms), inc))
        L.append('')

    with open(out_c, 'w', encoding='utf-8') as f:
        f.write('\n'.join(L))

    H = []
    H.append('/* 由 tools/gen_registry.py 生成，勿手改。 */')
    H.append('#ifndef %s' % guard)
    H.append('#define %s' % guard)
    H.append('')
    if names:
        H.append('#include "cxf/type.h"')
    H.append('#include "cxf/service.h"')
    H.append('')
    for n in names:
        H.append('extern const cxf_type cxf_type_%s;' % gen.structs[n]['cname_key'])
    if names:
        H.append('')
        H.append('/* 注册期：装配期整组登记，调用期不再查找。 */')
        H.append('extern const cxf_type_bundle cxf_types_%s;' % stem)
    if svc_syms:
        H.append('')
        H.append('/* 注册期：同上。服务查出来的是一张函数指针表。 */')
        H.append('extern const cxf_service_bundle cxf_services_%s;' % stem)
    H.append('')
    H.append('#endif')
    with open(out_h, 'w', encoding='utf-8') as f:
        f.write('\n'.join(H) + '\n')


def emit_types(gen, names, stem, src, L):
    """类型描述符本体。生成物只写 sizeof / _Alignof / offsetof，不写数值。"""

    # 宽度假设：平台不符就编译失败，而不是静默错标
    if gen.builtins:
        L.append('/* 内置类型的宽度假设。不符就编译失败，不静默错标。 */')
        for canon in sorted(gen.builtins):
            kind, width = BUILTIN[canon]
            L.append('_Static_assert(sizeof(%s) == %d, "cxf: %s 宽度与 %s 不符");'
                     % (canon, width, canon, kind))
        L.append('')

    L.append('/* 结构体描述符的前向声明：自引用与相互引用都要先有名字。 */')
    for n in names:
        L.append('extern const cxf_type cxf_type_%s;' % gen.structs[n]['cname_key'])
    L.append('')

    if gen.builtins:
        for canon in sorted(gen.builtins):
            var = gen.builtins[canon]
            kind, _ = BUILTIN[canon]
            L.append('static const cxf_type %s = { "%s", (uint32_t)sizeof(%s), '
                     '(uint32_t)_Alignof(%s), %s, 0, NULL, NULL };'
                     % (var, canon, canon, canon, kind))
        L.append('')

    if gen.ptrs:
        for pointee in sorted(gen.ptrs):
            p = gen.ptrs[pointee]
            expr = pointee + ' *'
            L.append('static const cxf_type %s = { "%s", (uint32_t)sizeof(%s), '
                     '(uint32_t)_Alignof(%s), CXF_K_PTR, 0, NULL, %s };'
                     % (p['var'], p['display'], expr, expr, p['target'] or 'NULL'))
        L.append('')

    for n in names:
        s = gen.structs[n]
        L.append('static const cxf_field cxf_fields_%s[] = {' % s['cname_key'])
        for fname, ref in s['fields']:
            L.append('    { "%s", (uint32_t)offsetof(%s, %s), &%s },'
                     % (fname, s['cname'], fname, ref))
        L.append('};')
        L.append('')

    for n in names:
        s = gen.structs[n]
        L.append('const cxf_type cxf_type_%s = { "%s", (uint32_t)sizeof(%s), '
                 '(uint32_t)_Alignof(%s), CXF_K_STRUCT, %d, cxf_fields_%s, NULL };'
                 % (s['cname_key'], n, s['cname'], s['cname'],
                    len(s['fields']), s['cname_key']))
        L.append('')

    # 成组注册表。与描述符同源产出 —— 一个头文件里几百个类型时，
    # 手写注册代码必然与描述符脱节。
    L.append('/* 注册期：整组登记进 world，见 type.h 的注册期说明。 */')
    L.append('static const cxf_type_entry cxf_entries_%s[] = {' % stem)
    for n in names:
        L.append('    { "%s", &cxf_type_%s },' % (n, gen.structs[n]['cname_key']))
    L.append('};')
    L.append('const cxf_type_bundle cxf_types_%s = '
             '{ cxf_entries_%s, %d, "%s" };' % (stem, stem, len(names), src))
    L.append('')


def leaves_of(gen, name, depth=0):
    """只为打印摘要：模拟展平，看每个类型最终会有多少列。"""
    s = gen.structs[name]
    n = 0
    for fname, ref in s['fields']:
        if ref.startswith('cxf_type_'):
            inner = ref[len('cxf_type_'):]
            for k, v in gen.structs.items():
                if v['cname_key'] == inner:
                    n += leaves_of(gen, k, depth + 1)
                    break
        else:
            n += 1
    return n


def main():
    ap = argparse.ArgumentParser(
        description='从 C 源码生成注册期产物（类型描述符 + 服务注册项）')
    ap.add_argument('source')
    ap.add_argument('-I', '--include', action='append', default=[])
    ap.add_argument('-o', '--out', help='输出的 .c')
    ap.add_argument('-H', '--out-header', help='输出的 .h')
    ap.add_argument('--std', default='c11')
    a = ap.parse_args()

    src = a.source
    base = re.sub(r'\.(h|c)$', '', src)
    out_c = a.out or base + '.gen.c'
    out_h = a.out_header or base + '.gen.h'

    types = scan_markers(src)
    svcs = list(dict.fromkeys(scan_service_markers(src)))
    if not types and not svcs:
        die('%s 里既没有 CXF_TYPE 也没有 CXF_SERVICE 标记' % src)

    gen = None
    if types:
        # 只有类型需要 clang。服务的全部信息都在宏参数里，跑一次 clang 是白跑。
        gen = Gen(Index(dump_ast(src, a.include, a.std)))
        for name in types:
            gen.require(name)

    emit(gen, svcs, src, out_c, out_h)

    sys.stdout.write('%s -> %s, %s\n' % (src, out_c, out_h))
    if gen is not None:
        for n in gen.structs:
            sys.stdout.write('  type %-12s 字段 %2d  叶子 %2d\n'
                             % (n, len(gen.structs[n]['fields']), leaves_of(gen, n)))
        auto = [n for n in gen.structs if n not in types]
        if auto:
            sys.stdout.write('  依赖自动带入：%s\n' % ', '.join(auto))
    for s in svcs:
        sys.stdout.write('  service cxf_svc_%s\n' % s)


if __name__ == '__main__':
    main()
