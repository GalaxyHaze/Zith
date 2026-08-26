#include "unify.hpp"
#include "common/overloaded.hpp"
#include "diagnostics/error-codes.hpp"
#include "types/type-walker.hpp"

#include <span>
#include <variant>

namespace zith::types {

Unifier::Unifier(TypeIntern &intern, diagnostics::DiagnosticEngine &diags, memory::Arena &arena)
    : intern_(intern), diags_(diags), subst_(arena) {}

TypeId Unifier::freshVar() {
    return intern_.internTypeVar();
}

TypeId Unifier::substitute(TypeId t) const {
    for (;;) {
        if (t >= subst_.size())
            return t;
        TypeId next = subst_[t];
        if (next == t || next == kInvalidType)
            return t;
        t = next;
    }
}

bool Unifier::occurs(TypeId var, TypeId t) const {
    TypeId resolved = substitute(t);
    if (resolved == var)
        return true;
    if (resolved >= intern_.count())
        return false;

    auto &data = intern_.lookup(resolved);
    bool found = false;
    walkSubTypes(data, [&](TypeId child) {
        if (!found && occurs(var, child))
            found = true;
    });
    return found;
}

bool Unifier::unify(TypeId a, TypeId b, memory::Span span) {
    a = substitute(a);
    b = substitute(b);

    if (a == b)
        return true;

    auto kind_a = intern_.kindOf(a);
    auto kind_b = intern_.kindOf(b);

    if (kind_a == TypeKind::TypeVar) {
        if (occurs(a, b)) {
            diags_.report(diagnostics::Severity::Error, diagnostics::err::CyclicType,
                          "cyclic type: type variable contains itself", span);
            return false;
        }
        if (a >= subst_.size())
            subst_.reserve(a + 1);
        while (subst_.size() <= a)
            subst_.push(kInvalidType);
        subst_[a] = b;
        return true;
    }

    if (kind_b == TypeKind::TypeVar) {
        if (occurs(b, a)) {
            diags_.report(diagnostics::Severity::Error, diagnostics::err::CyclicType,
                          "cyclic type: type variable contains itself", span);
            return false;
        }
        if (b >= subst_.size())
            subst_.reserve(b + 1);
        while (subst_.size() <= b)
            subst_.push(kInvalidType);
        subst_[b] = a;
        return true;
    }

    if (kind_a != kind_b) {
        diags_.report(diagnostics::Severity::Error, diagnostics::err::TypeMismatch,
                      "type mismatch: cannot unify types of different kinds", span);
        return false;
    }

    auto &data_a = intern_.lookup(a);
    auto &data_b = intern_.lookup(b);

    return visitType(
        data_a, data_b,
        common::overloaded{
            [&](const TypeInt &ai, const TypeInt &bi) -> bool {
                if (ai.width == bi.width)
                    return true;
                if (ai.width == IntWidth::Literal || bi.width == IntWidth::Literal)
                    return true;
                diags_.report(diagnostics::Severity::Error, diagnostics::err::TypeMismatch,
                              "type mismatch: integer width differs", span);
                return false;
            },
            [&](const TypeFloat &af, const TypeFloat &bf) -> bool {
                if (af.width != bf.width) {
                    diags_.report(diagnostics::Severity::Error, diagnostics::err::TypeMismatch,
                                  "type mismatch: float width differs", span);
                    return false;
                }
                return true;
            },
            [&](const TypePtr &pa, const TypePtr &pb) -> bool {
                if (pa.is_mut != pb.is_mut) {
                    diags_.report(diagnostics::Severity::Error, diagnostics::err::TypeMismatch,
                                  "type mismatch: mutability differs", span);
                    return false;
                }
                if (pa.ownership != pb.ownership) {
                    diags_.report(diagnostics::Severity::Error, diagnostics::err::TypeMismatch,
                                  "type mismatch: ownership qualifier differs", span);
                    return false;
                }
                return unify(pa.pointee, pb.pointee, span);
            },
            [&](const TypeArray &aa, const TypeArray &ab) -> bool {
                if (aa.count != ab.count) {
                    diags_.report(diagnostics::Severity::Error, diagnostics::err::TypeMismatch,
                                  "type mismatch: array length differs", span);
                    return false;
                }
                return unify(aa.elem, ab.elem, span);
            },
            [&](const TypeStruct &sa, const TypeStruct &sb) -> bool {
                if (sa.def_id != sb.def_id) {
                    diags_.report(diagnostics::Severity::Error, diagnostics::err::TypeMismatch,
                                  "type mismatch: different struct types", span);
                    return false;
                }
                return true;
            },
            [&](const TypeFn &fa, const TypeFn &fb) -> bool {
                if (fa.param_count != fb.param_count) {
                    diags_.report(diagnostics::Severity::Error, diagnostics::err::TypeMismatch,
                                  "type mismatch: function parameter count differs", span);
                    return false;
                }
                if (!unify(fa.ret, fb.ret, span))
                    return false;
                for (size_t i = 0; i < fa.param_count; i++) {
                    if (!unify(fa.params[i], fb.params[i], span))
                        return false;
                }
                return true;
            },
            [&](const TypeOptional &oa, const TypeOptional &ob) -> bool {
                return unify(oa.inner, ob.inner, span);
            },
            [&](const TypeFailable &fa, const TypeFailable &fb) -> bool {
                return unify(fa.inner, fb.inner, span);
            },
            [&](const TypeAlias &aa, const TypeAlias &ab) -> bool {
                return unify(aa.target, ab.target, span);
            },
            [&](const TypeNominal &na, const TypeNominal &nb) -> bool {
                return na.name == nb.name && unify(na.target, nb.target, span);
            },
            [&](const TypeTrait &ta, const TypeTrait &tb) -> bool { return ta.name == tb.name; },
            [&](const TypeQualified &qa, const TypeQualified &qb) -> bool {
                if (qa.ownership != qb.ownership || qa.isMut != qb.isMut)
                    return false;
                return unify(qa.inner, qb.inner, span);
            },
            [&](const TypeSlice &sa, const TypeSlice &sb) -> bool {
                return unify(sa.elem, sb.elem, span);
            },
            [&](const TypeEnum &ea, const TypeEnum &eb) -> bool {
                if (ea.def_id != eb.def_id) {
                    diags_.report(diagnostics::Severity::Error, diagnostics::err::TypeMismatch,
                                  "type mismatch: different enum types", span);
                    return false;
                }
                return true;
            },
            [&](const TypeUnion &ua, const TypeUnion &ub) -> bool {
                if (ua.def_id != ub.def_id) {
                    diags_.report(diagnostics::Severity::Error, diagnostics::err::TypeMismatch,
                                  "type mismatch: different union types", span);
                    return false;
                }
                return true;
            },
            [&](const TypePack &pa, const TypePack &pb) -> bool {
                if (pa.count != pb.count) {
                    diags_.report(diagnostics::Severity::Error, diagnostics::err::TypeMismatch,
                                  "type mismatch: pack member count differs", span);
                    return false;
                }
                for (size_t i = 0; i < pa.count; i++) {
                    if (!unify(pa.members[i], pb.members[i], span))
                        return false;
                }
                return true;
            },
            [&](const TypeSum &sa, const TypeSum &sb) -> bool {
                if (sa.count != sb.count) {
                    diags_.report(diagnostics::Severity::Error, diagnostics::err::TypeMismatch,
                                  "type mismatch: sum member count differs", span);
                    return false;
                }
                for (size_t i = 0; i < sa.count; i++) {
                    if (!unify(sa.members[i], sb.members[i], span))
                        return false;
                }
                return true;
            },
            [&](const TypeGenericParam &ga, const TypeGenericParam &gb) -> bool {
                if (ga.decl_id != gb.decl_id || ga.param_index != gb.param_index) {
                    diags_.report(diagnostics::Severity::Error, diagnostics::err::TypeMismatch,
                                  "type mismatch: different generic parameters", span);
                    return false;
                }
                return true;
            },
            [&](const TypeIncomplete &ia, const TypeIncomplete &ib) -> bool {
                if (ia.base != ib.base || ia.arg_count != ib.arg_count) {
                    diags_.report(diagnostics::Severity::Error, diagnostics::err::TypeMismatch,
                                  "type mismatch: incomplete type structure differs", span);
                    return false;
                }
                for (size_t i = 0; i < ia.arg_count; i++) {
                    if (!unify(ia.args[i], ib.args[i], span))
                        return false;
                }
                return true;
            },
            // Bool, Char, Error, Never, Void, Opaque, Unknown, TypeVar — same kind means equal
            [](const auto &, const auto &) -> bool { return true; },
        });
}

bool Unifier::isAssignable(TypeId dst, TypeId src) const {
    dst = substitute(dst);
    src = substitute(src);

    if (dst == src)
        return true;

    auto kind_dst = intern_.kindOf(dst);
    auto kind_src = intern_.kindOf(src);

    if (kind_dst == TypeKind::TypeVar || kind_src == TypeKind::TypeVar)
        return true;

    if (kind_dst != kind_src) {
        if (kind_src == TypeKind::Never)
            return true;
        if (kind_dst == TypeKind::Bool && kind_src == TypeKind::Int) {
            auto *src_int = std::get_if<TypeInt>(&intern_.lookup(src));
            return src_int->width == IntWidth::I8 || src_int->width == IntWidth::U8;
        }
        return false;
    }

    auto &data_dst = intern_.lookup(dst);
    auto &data_src = intern_.lookup(src);

    return visitType(
        data_dst, data_src,
        common::overloaded{
            [&](const TypeInt &di, const TypeInt &si) -> bool {
                if (di.width == si.width)
                    return true;
                if (di.width == IntWidth::Literal || si.width == IntWidth::Literal)
                    return true;
                return false;
            },
            [&](const TypeFloat &df, const TypeFloat &sf) -> bool { return df.width == sf.width; },
            [&](const TypePtr &pd, const TypePtr &ps) -> bool {
                if (pd.ownership != ps.ownership)
                    return false;
                if (pd.is_mut && !ps.is_mut)
                    return false;
                return isAssignable(pd.pointee, ps.pointee);
            },
            [&](const TypeArray &ad, const TypeArray &as) -> bool {
                if (ad.count != as.count && ad.count != 0 && as.count != 0)
                    return false;
                return isAssignable(ad.elem, as.elem);
            },
            [&](const TypeStruct &sd, const TypeStruct &ss) -> bool {
                return sd.def_id == ss.def_id;
            },
            [&](const TypeFn &fd, const TypeFn &fs) -> bool {
                if (fd.param_count != fs.param_count)
                    return false;
                if (!isAssignable(fd.ret, fs.ret))
                    return false;
                for (size_t i = 0; i < fd.param_count; i++)
                    if (!isAssignable(fd.params[i], fs.params[i]))
                        return false;
                return true;
            },
            [&](const TypeOptional &od, const TypeOptional &os) -> bool {
                return isAssignable(od.inner, os.inner);
            },
            [&](const TypeFailable &fd, const TypeFailable &fs) -> bool {
                return isAssignable(fd.inner, fs.inner);
            },
            [&](const TypeAlias &ad, const TypeAlias &as) -> bool {
                return isAssignable(ad.target, as.target);
            },
            [&](const TypeNominal &nd, const TypeNominal &ns) -> bool {
                return nd.name == ns.name && isAssignable(nd.target, ns.target);
            },
            [&](const TypeTrait &td, const TypeTrait &ts) -> bool { return td.name == ts.name; },
            [&](const TypeQualified &qd, const TypeQualified &qs) -> bool {
                if (qd.ownership != qs.ownership || qd.isMut != qs.isMut)
                    return false;
                return isAssignable(qd.inner, qs.inner);
            },
            [&](const TypeSlice &sd, const TypeSlice &ss) -> bool {
                return isAssignable(sd.elem, ss.elem);
            },
            [&](const TypeEnum &ed, const TypeEnum &es) -> bool { return ed.def_id == es.def_id; },
            [&](const TypeUnion &ud, const TypeUnion &us) -> bool {
                return ud.def_id == us.def_id;
            },
            [&](const TypePack &pd, const TypePack &ps) -> bool {
                if (pd.count != ps.count)
                    return false;
                for (size_t i = 0; i < pd.count; i++)
                    if (!isAssignable(pd.members[i], ps.members[i]))
                        return false;
                return true;
            },
            [&](const TypeSum &sd, const TypeSum &ss) -> bool {
                if (sd.count != ss.count)
                    return false;
                for (size_t i = 0; i < sd.count; i++)
                    if (!isAssignable(sd.members[i], ss.members[i]))
                        return false;
                return true;
            },
            [&](const TypeGenericParam &gd, const TypeGenericParam &gs) -> bool {
                return gd.decl_id == gs.decl_id && gd.param_index == gs.param_index;
            },
            [&](const TypeIncomplete &idd, const TypeIncomplete &iss) -> bool {
                if (idd.base != iss.base || idd.arg_count != iss.arg_count)
                    return false;
                for (size_t i = 0; i < idd.arg_count; i++)
                    if (!isAssignable(idd.args[i], iss.args[i]))
                        return false;
                return true;
            },
            [](const TypeUnknown &, const TypeUnknown &) -> bool { return true; },
            [](const TypeNever &, const TypeNever &) -> bool { return true; },
            [](const auto &, const auto &) -> bool { return false; },
        });
}

bool Unifier::isCoercible(TypeId dst, TypeId src) const {
    dst = substitute(dst);
    src = substitute(src);

    if (isAssignable(dst, src))
        return true;

    auto kind_dst = intern_.kindOf(dst);
    auto kind_src = intern_.kindOf(src);

    if (kind_dst == TypeKind::Optional) {
        auto *opt = std::get_if<TypeOptional>(&intern_.lookup(dst));
        return isCoercible(opt->inner, src);
    }

    if (kind_dst == TypeKind::Int && kind_src == TypeKind::Int) {
        auto *d    = std::get_if<TypeInt>(&intern_.lookup(dst));
        auto *s    = std::get_if<TypeInt>(&intern_.lookup(src));
        auto w_dst = d->width;
        auto w_src = s->width;
        if (w_dst == IntWidth::Literal || w_src == IntWidth::Literal)
            return true;
        auto to_u = [](IntWidth w) -> uint8_t {
            switch (w) {
            case IntWidth::I8:
            case IntWidth::U8:
                return 8;
            case IntWidth::I16:
            case IntWidth::U16:
                return 16;
            case IntWidth::I32:
            case IntWidth::U32:
                return 32;
            case IntWidth::I64:
            case IntWidth::U64:
                return 64;
            case IntWidth::I128:
            case IntWidth::U128:
                return 128;
            case IntWidth::Literal:
                return 0;
            }
            return 0;
        };
        return to_u(w_dst) >= to_u(w_src);
    }

    if (kind_dst == TypeKind::Float && kind_src == TypeKind::Float) {
        auto *d    = std::get_if<TypeFloat>(&intern_.lookup(dst));
        auto *s    = std::get_if<TypeFloat>(&intern_.lookup(src));
        auto w_dst = d->width;
        auto w_src = s->width;
        return w_dst >= w_src;
    }

    return kind_dst == kind_src;
}

} // namespace zith::types
