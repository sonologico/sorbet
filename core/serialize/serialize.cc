#include "core/serialize/serialize.h"
#include "absl/base/casts.h"
#include "absl/types/span.h"
#include "ast/Helpers.h"
#include "common/Timer.h"
#include "common/sort.h"
#include "common/typecase.h"
#include "core/Error.h"
#include "core/GlobalState.h"
#include "core/NameHash.h"
#include "core/Symbols.h"
#include "core/serialize/pickler.h"
#include "lib/lizard_compress.h"
#include "lib/lizard_decompress.h"

template class std::vector<sorbet::u4>;

using namespace std;

namespace sorbet::core::serialize {
const u4 Serializer::VERSION;
const u1 Serializer::GLOBAL_STATE_COMPRESSION_DEGREE;
const u1 Serializer::FILE_COMPRESSION_DEGREE;

// These helper methods are declared in a class inside of an anonymous namespace
// or inline so that `GlobalState` can forward-declare and `friend` the entire
// class.
class SerializerImpl {
public:
    static Pickler pickle(const GlobalState &gs, bool payloadOnly = false);
    static void pickle(Pickler &p, const File &what);
    static void pickle(Pickler &p, const Name &what);
    static void pickle(Pickler &p, Type *what);
    static void pickle(Pickler &p, const ArgInfo &a);
    static void pickle(Pickler &p, const Symbol &what);
    static void pickle(Pickler &p, const ast::TreePtr &what);
    static void pickle(Pickler &p, core::LocOffsets loc);
    static void pickleWithFile(Pickler &p, core::Loc loc);
    static void pickleWithoutFile(Pickler &p, core::Loc loc);
    static void pickle(Pickler &p, shared_ptr<const FileHash> fh);

    static void pickleTree(Pickler &p, ast::TreePtr &t);

    static shared_ptr<File> unpickleFile(UnPickler &p);
    static Name unpickleName(UnPickler &p, GlobalState &gs);
    static TypePtr unpickleType(UnPickler &p, const GlobalState *gs);
    static ArgInfo unpickleArgInfo(UnPickler &p, const GlobalState *gs);
    static Symbol unpickleSymbol(UnPickler &p, const GlobalState *gs);
    static void unpickleGS(UnPickler &p, GlobalState &result);
    static u4 unpickleGSUUID(UnPickler &p);
    static Loc unpickleLocInFile(UnPickler &p, FileRef file);
    static LocOffsets unpickleLocOffsets(UnPickler &p);
    static Loc unpickleLoc(UnPickler &p);
    static ast::TreePtr unpickleExpr(UnPickler &p, const GlobalState &, FileRef file);
    static NameRef unpickleNameRef(UnPickler &p, const GlobalState &);
    static NameRef unpickleNameRef(UnPickler &p, GlobalState &);
    static unique_ptr<const FileHash> unpickleFileHash(UnPickler &p);

    SerializerImpl() = delete;

private:
    static void pickleAstHeader(Pickler &p, u1 tag, ast::Expression *tree);
};

void Pickler::putStr(string_view s) {
    putU4(s.size());

    for (char c : s) {
        putU1(absl::bit_cast<u1>(c));
    }
}

constexpr size_t SIZE_BYTES = sizeof(int) / sizeof(u1);

vector<u1> Pickler::result(int compressionDegree) {
    if (zeroCounter != 0) {
        data.emplace_back(zeroCounter);
        zeroCounter = 0;
    }
    const size_t maxDstSize = Lizard_compressBound(data.size());
    vector<u1> compressedData;
    compressedData.resize(2048 + maxDstSize); // give extra room for compression
                                              // Lizard_compressBound returns size of data if compression
                                              // succeeds. It seems to be written for big inputs
                                              // and returns too small sizes for small inputs,
                                              // where compressed size is bigger than original size
    int resultCode = Lizard_compress((const char *)data.data(), (char *)(compressedData.data() + SIZE_BYTES * 2),
                                     data.size(), (compressedData.size() - SIZE_BYTES * 2), compressionDegree);
    if (resultCode == 0) {
        // did not compress!
        Exception::raise("incompressible pickler?");
    } else {
        memcpy(compressedData.data(), &resultCode, SIZE_BYTES); // ~200K of our stdlib
        int uncompressedSize = data.size();
        memcpy(compressedData.data() + SIZE_BYTES, &uncompressedSize,
               SIZE_BYTES);                                     // 172817 ints(x4), ~675K of our stdlib
        int actualCompressedSize = resultCode + SIZE_BYTES * 2; // SIZE_BYTES * 2 are for sizes
        compressedData.resize(actualCompressedSize);
    }
    return compressedData;
}

UnPickler::UnPickler(const u1 *const compressed, spdlog::logger &tracer) : pos(0) {
    Timer timeit(tracer, "Unpickler::UnPickler");
    int compressedSize;
    memcpy(&compressedSize, compressed, SIZE_BYTES);
    int uncompressedSize;
    memcpy(&uncompressedSize, compressed + SIZE_BYTES, SIZE_BYTES);

    data.resize(uncompressedSize);

    int resultCode = Lizard_decompress_safe((const char *)(compressed + 2 * SIZE_BYTES), (char *)this->data.data(),
                                            compressedSize, uncompressedSize);
    if (resultCode != uncompressedSize) {
        Exception::raise("incomplete decompression");
    }
}

string_view UnPickler::getStr() {
    int sz = getU4();
    string_view result((char *)&data[pos], sz);
    pos += sz;

    return result;
}

void Pickler::putU1(u1 u) {
    if (zeroCounter != 0) {
        data.emplace_back(zeroCounter);
        zeroCounter = 0;
    }
    data.emplace_back(u);
}

u1 UnPickler::getU1() {
    ENFORCE(zeroCounter == 0);
    auto res = data[pos++];
    return res;
}

void Pickler::putU4(u4 u) {
    if (u == 0) {
        if (zeroCounter != 0) {
            if (zeroCounter == UCHAR_MAX) {
                data.emplace_back(UCHAR_MAX);
                zeroCounter = 0;
                putU4(u);
                return;
            }
            zeroCounter++;
            return;
        } else {
            data.emplace_back(0);
            zeroCounter = 1;
        }
    } else {
        if (zeroCounter != 0) {
            data.emplace_back(zeroCounter);
            zeroCounter = 0;
        }
        while (u > 127) {
            data.emplace_back(128 | (u & 127));
            u = u >> 7;
        }
        data.emplace_back(u & 127);
    }
}

u4 UnPickler::getU4() {
    if (zeroCounter != 0) {
        zeroCounter--;
        return 0;
    }
    u1 r = data[pos++];
    if (r == 0) {
        zeroCounter = data[pos++];
        zeroCounter--;
        return r;
    } else {
        u4 res = r & 127;
        u4 vle = r;
        if ((vle & 128) == 0) {
            goto done;
        }

        vle = data[pos++];
        res |= (vle & 127) << 7;
        if ((vle & 128) == 0) {
            goto done;
        }

        vle = data[pos++];
        res |= (vle & 127) << 14;
        if ((vle & 128) == 0) {
            goto done;
        }

        vle = data[pos++];
        res |= (vle & 127) << 21;
        if ((vle & 128) == 0) {
            goto done;
        }

        vle = data[pos++];
        res |= (vle & 127) << 28;
        if ((vle & 128) == 0) {
            goto done;
        }

    done:
        return res;
    }
}

void Pickler::putS8(const int64_t i) {
    auto u = absl::bit_cast<u8>(i);
    while (u > 127) {
        putU1((u & 127) | 128);
        u = u >> 7;
    }
    putU1(u & 127);
}

int64_t UnPickler::getS8() {
    u8 res = 0;
    u8 vle = 128;
    int i = 0;
    while (vle & 128) {
        vle = getU1();
        res |= (vle & 127) << (i * 7);
        i++;
    }
    return absl::bit_cast<int64_t>(res);
}

void SerializerImpl::pickle(Pickler &p, shared_ptr<const FileHash> fh) {
    if (fh == nullptr) {
        p.putU1(0);
        return;
    }
    p.putU1(1);
    p.putU4(fh->definitions.hierarchyHash);
    p.putU4(fh->definitions.methodHashes.size());
    for (const auto &[key, value] : fh->definitions.methodHashes) {
        p.putU4(key._hashValue);
        p.putU4(value);
    }
    p.putU4(fh->usages.constants.size());
    for (const auto &e : fh->usages.constants) {
        p.putU4(e._hashValue);
    }
    p.putU4(fh->usages.sends.size());
    for (const auto &e : fh->usages.sends) {
        p.putU4(e._hashValue);
    }
}

unique_ptr<const FileHash> SerializerImpl::unpickleFileHash(UnPickler &p) {
    auto hadIt = p.getU1() != 0;
    if (!hadIt) {
        return nullptr;
    }
    FileHash ret;

    ret.definitions.hierarchyHash = p.getU4();
    auto methodHashSize = p.getU4();
    ret.definitions.methodHashes.reserve(methodHashSize);
    for (int it = 0; it < methodHashSize; it++) {
        NameHash key;
        key._hashValue = p.getU4();
        ret.definitions.methodHashes.emplace_back(key, p.getU4());
    }
    auto constantsSize = p.getU4();
    ret.usages.constants.reserve(constantsSize);
    for (int it = 0; it < constantsSize; it++) {
        NameHash key;
        key._hashValue = p.getU4();
        ret.usages.constants.emplace_back(key);
    }
    auto sendsSize = p.getU4();
    ret.usages.sends.reserve(sendsSize);
    for (int it = 0; it < sendsSize; it++) {
        NameHash key;
        key._hashValue = p.getU4();
        ret.usages.sends.emplace_back(key);
    }
    return make_unique<const FileHash>(move(ret));
}

void SerializerImpl::pickle(Pickler &p, const File &what) {
    p.putU1((u1)what.sourceType);
    p.putStr(what.path());
    p.putStr(what.source());
    pickle(p, what.getFileHash());
}

shared_ptr<File> SerializerImpl::unpickleFile(UnPickler &p) {
    auto t = (File::Type)p.getU1();
    auto path = string(p.getStr());
    auto source = string(p.getStr());
    auto globalStateHash = SerializerImpl::unpickleFileHash(p);
    auto ret = make_shared<File>(std::move(path), std::move(source), t);
    ret->setFileHash(move(globalStateHash));
    return ret;
}

void SerializerImpl::pickle(Pickler &p, const Name &what) {
    p.putU1(static_cast<u1>(what.kind));
    switch (what.kind) {
        case NameKind::UTF8:
            p.putStr(what.raw.utf8);
            break;
        case NameKind::UNIQUE:
            p.putU1(static_cast<u1>(what.unique.uniqueNameKind));
            p.putU4(what.unique.original._id);
            p.putU4(what.unique.num);
            break;
        case NameKind::CONSTANT:
            p.putU4(what.cnst.original.id());
            break;
    }
}

Name SerializerImpl::unpickleName(UnPickler &p, GlobalState &gs) {
    Name result;
    result.kind = (NameKind)p.getU1();
    switch (result.kind) {
        case NameKind::UTF8:
            result.kind = NameKind::UTF8;
            result.raw.utf8 = gs.enterString(p.getStr());
            break;
        case NameKind::UNIQUE:
            result.unique.uniqueNameKind = (UniqueNameKind)p.getU1();
            result.unique.original = NameRef(gs, p.getU4());
            result.unique.num = p.getU4();
            break;
        case NameKind::CONSTANT:
            result.cnst.original = NameRef(gs, p.getU4());
            break;
    }
    return result;
}

void SerializerImpl::pickle(Pickler &p, Type *what) {
    if (what == nullptr) {
        p.putU4(0);
        return;
    }
    if (auto *c = cast_type<ClassType>(what)) {
        p.putU4(1);
        p.putU4(c->symbol._id);
    } else if (auto *o = cast_type<OrType>(what)) {
        p.putU4(2);
        pickle(p, o->left.get());
        pickle(p, o->right.get());
    } else if (auto *c = cast_type<LiteralType>(what)) {
        p.putU4(3);
        p.putU1((u1)c->literalKind);
        p.putS8(c->value);
    } else if (auto *a = cast_type<AndType>(what)) {
        p.putU4(4);
        pickle(p, a->left.get());
        pickle(p, a->right.get());
    } else if (auto *arr = cast_type<TupleType>(what)) {
        p.putU4(5);
        pickle(p, arr->underlying().get());
        p.putU4(arr->elems.size());
        for (auto &el : arr->elems) {
            pickle(p, el.get());
        }
    } else if (auto *hash = cast_type<ShapeType>(what)) {
        p.putU4(6);
        pickle(p, hash->underlying().get());
        p.putU4(hash->keys.size());
        ENFORCE(hash->keys.size() == hash->values.size());
        for (auto &el : hash->keys) {
            pickle(p, el.get());
        }
        for (auto &el : hash->values) {
            pickle(p, el.get());
        }
    } else if (auto *alias = cast_type<AliasType>(what)) {
        p.putU4(7);
        p.putU4(alias->symbol._id);
    } else if (auto *lp = cast_type<LambdaParam>(what)) {
        p.putU4(8);
        pickle(p, lp->lowerBound.get());
        pickle(p, lp->upperBound.get());
        p.putU4(lp->definition._id);
    } else if (auto *at = cast_type<AppliedType>(what)) {
        p.putU4(9);
        p.putU4(at->klass._id);
        p.putU4(at->targs.size());
        for (auto &t : at->targs) {
            pickle(p, t.get());
        }
    } else if (auto *tp = cast_type<TypeVar>(what)) {
        p.putU4(10);
        p.putU4(tp->sym._id);
    } else if (auto *st = cast_type<SelfType>(what)) {
        p.putU4(11);
    } else {
        Exception::notImplemented();
    }
}

TypePtr SerializerImpl::unpickleType(UnPickler &p, const GlobalState *gs) {
    auto tag = p.getU4(); // though we formally need only u1 here, benchmarks suggest that size difference after
                          // compression is small and u4 is 10% faster
    switch (tag) {
        case 0: {
            TypePtr empty;
            return empty;
        }
        case 1:
            return make_type<ClassType>(SymbolRef(gs, p.getU4()));
        case 2:
            return OrType::make_shared(unpickleType(p, gs), unpickleType(p, gs));
        case 3: {
            auto kind = (core::LiteralType::LiteralTypeKind)p.getU1();
            auto value = p.getS8();
            switch (kind) {
                case LiteralType::LiteralTypeKind::Integer:
                    return make_type<LiteralType>(value);
                case LiteralType::LiteralTypeKind::Float:
                    return make_type<LiteralType>(absl::bit_cast<double>(value));
                case LiteralType::LiteralTypeKind::String:
                    return make_type<LiteralType>(Symbols::String(), core::NameRef(NameRef::WellKnown{}, value));
                case LiteralType::LiteralTypeKind::Symbol:
                    return make_type<LiteralType>(Symbols::Symbol(), core::NameRef(NameRef::WellKnown{}, value));
                case LiteralType::LiteralTypeKind::True:
                    return make_type<LiteralType>(true);
                case LiteralType::LiteralTypeKind::False:
                    return make_type<LiteralType>(false);
            }
            Exception::notImplemented();
        }
        case 4:
            return AndType::make_shared(unpickleType(p, gs), unpickleType(p, gs));
        case 5: {
            auto underlying = unpickleType(p, gs);
            int sz = p.getU4();
            vector<TypePtr> elems(sz);
            for (auto &elem : elems) {
                elem = unpickleType(p, gs);
            }
            auto result = make_type<TupleType>(underlying, std::move(elems));
            return result;
        }
        case 6: {
            auto underlying = unpickleType(p, gs);
            int sz = p.getU4();
            vector<TypePtr> keys(sz);
            vector<TypePtr> values(sz);
            for (auto &key : keys) {
                key = unpickleType(p, gs);
            }
            for (auto &value : values) {
                value = unpickleType(p, gs);
            }
            auto result = make_type<ShapeType>(underlying, keys, values);
            return result;
        }
        case 7:
            return make_type<AliasType>(SymbolRef(gs, p.getU4()));
        case 8: {
            auto lower = unpickleType(p, gs);
            auto upper = unpickleType(p, gs);
            return make_type<LambdaParam>(SymbolRef(gs, p.getU4()), lower, upper);
        }
        case 9: {
            SymbolRef klass(gs, p.getU4());
            int sz = p.getU4();
            vector<TypePtr> targs(sz);
            for (auto &t : targs) {
                t = unpickleType(p, gs);
            }
            return make_type<AppliedType>(klass, targs);
        }
        case 10: {
            SymbolRef sym(gs, p.getU4());
            return make_type<TypeVar>(sym);
        }
        case 11: {
            return make_type<SelfType>();
        }
        default:
            Exception::raise("Unknown type tag {}", tag);
    }
}

void SerializerImpl::pickle(Pickler &p, const ArgInfo &a) {
    p.putU4(a.name._id);
    p.putU4(a.rebind._id);
    pickleWithFile(p, a.loc);
    p.putU1(a.flags.toU1());
    pickle(p, a.type.get());
}

ArgInfo SerializerImpl::unpickleArgInfo(UnPickler &p, const GlobalState *gs) {
    ArgInfo result;
    result.name = core::NameRef(*gs, p.getU4());
    result.rebind = core::SymbolRef(gs, p.getU4());
    result.loc = unpickleLoc(p);
    {
        u1 flags = p.getU1();
        result.flags.setFromU1(flags);
    }
    result.type = unpickleType(p, gs);
    return result;
}

void SerializerImpl::pickle(Pickler &p, const Symbol &what) {
    p.putU4(what.owner._id);
    p.putU4(what.name._id);
    p.putU4(what.superClassOrRebind._id);
    p.putU4(what.flags);
    if (!what.isMethod()) {
        p.putU4(what.mixins_.size());
        for (SymbolRef s : what.mixins_) {
            p.putU4(s._id);
        }
    }
    p.putU4(what.typeParams.size());
    for (SymbolRef s : what.typeParams) {
        p.putU4(s._id);
    }
    if (what.isMethod()) {
        p.putU4(what.arguments().size());
        for (const auto &a : what.arguments()) {
            pickle(p, a);
        }
    }
    p.putU4(what.members().size());
    vector<pair<u4, u4>> membersSorted;

    for (const auto &member : what.members()) {
        membersSorted.emplace_back(member.first.id(), member.second._id);
    }
    fast_sort(membersSorted, [](auto const &lhs, auto const &rhs) -> bool { return lhs.first < rhs.first; });

    for (const auto &member : membersSorted) {
        p.putU4(member.first);
        p.putU4(member.second);
    }

    pickle(p, what.resultType.get());
    p.putU4(what.locs().size());
    for (auto &loc : what.locs()) {
        pickleWithFile(p, loc);
    }
}

Symbol SerializerImpl::unpickleSymbol(UnPickler &p, const GlobalState *gs) {
    Symbol result;
    result.owner = SymbolRef(gs, p.getU4());
    result.name = NameRef(*gs, p.getU4());
    result.superClassOrRebind = SymbolRef(gs, p.getU4());
    result.flags = p.getU4();
    if (!result.isMethod()) {
        int mixinsSize = p.getU4();
        result.mixins_.reserve(mixinsSize);
        for (int i = 0; i < mixinsSize; i++) {
            result.mixins_.emplace_back(SymbolRef(gs, p.getU4()));
        }
    }
    int typeParamsSize = p.getU4();

    result.typeParams.reserve(typeParamsSize);
    for (int i = 0; i < typeParamsSize; i++) {
        result.typeParams.emplace_back(SymbolRef(gs, p.getU4()));
    }

    if (result.isMethod()) {
        int argsSize = p.getU4();
        for (int i = 0; i < argsSize; i++) {
            result.arguments().emplace_back(unpickleArgInfo(p, gs));
        }
    }
    int membersSize = p.getU4();
    result.members().reserve(membersSize);
    for (int i = 0; i < membersSize; i++) {
        auto name = NameRef(*gs, p.getU4());
        auto sym = SymbolRef(gs, p.getU4());
        if (result.name != core::Names::Constants::Root()) {
            ENFORCE(name.exists());
            ENFORCE(sym.exists());
        }
        result.members()[name] = sym;
    }
    result.resultType = unpickleType(p, gs);
    auto locCount = p.getU4();
    for (int i = 0; i < locCount; i++) {
        result.locs_.emplace_back(unpickleLoc(p));
    }
    return result;
}

Pickler SerializerImpl::pickle(const GlobalState &gs, bool payloadOnly) {
    Timer timeit(gs.tracer(), "pickleGlobalState");
    Pickler result;
    result.putU4(Serializer::VERSION);
    result.putU4(gs.kvstoreUuid);

    absl::Span<const shared_ptr<File>> wantFiles;
    if (payloadOnly) {
        auto lastPayload =
            absl::c_find_if(gs.files, [](auto &file) { return file && file->sourceType != File::Type::Payload; });
        ENFORCE(
            none_of(lastPayload, gs.files.end(), [](auto &file) { return file->sourceType == File::Type::Payload; }));
        wantFiles = absl::Span<const shared_ptr<File>>(gs.files.data(), lastPayload - gs.files.begin());
    } else {
        wantFiles = absl::Span<const shared_ptr<File>>(gs.files.data(), gs.files.size());
    }

    result.putU4(wantFiles.size());
    int i = -1;
    for (auto &f : wantFiles) {
        ++i;
        if (i != 0) {
            pickle(result, *f);
        }
    }

    result.putU4(gs.names.size());
    i = -1;
    for (const Name &n : gs.names) {
        ++i;
        if (i != 0) {
            pickle(result, n);
        }
    }

    result.putU4(gs.symbols.size());
    for (const Symbol &s : gs.symbols) {
        pickle(result, s);
    }

    result.putU4(gs.namesByHash.size());
    for (const auto &s : gs.namesByHash) {
        result.putU4(s.first);
        result.putU4(s.second);
    }
    return result;
}

int nearestPowerOf2(int from) {
    int i = 1;
    while (i < from) {
        i = i * 2;
    }
    return i;
}

u4 SerializerImpl::unpickleGSUUID(UnPickler &p) {
    if (p.getU4() != Serializer::VERSION) {
        Exception::raise("Payload version mismatch");
    }
    return p.getU4();
}

void SerializerImpl::unpickleGS(UnPickler &p, GlobalState &result) {
    Timer timeit(result.tracer(), "unpickleGS");
    result.creation = timeit.getFlowEdge();
    if (p.getU4() != Serializer::VERSION) {
        Exception::raise("Payload version mismatch");
    }

    result.kvstoreUuid = p.getU4();

    vector<shared_ptr<File>> files(std::move(result.files));
    files.clear();
    vector<Name> names(std::move(result.names));
    names.clear();
    vector<Symbol> symbols(std::move(result.symbols));
    symbols.clear();
    vector<pair<unsigned int, unsigned int>> namesByHash(std::move(result.namesByHash));
    namesByHash.clear();
    {
        Timer timeit(result.tracer(), "readFiles");

        int filesSize = p.getU4();
        files.reserve(filesSize);
        for (int i = 0; i < filesSize; i++) {
            if (i == 0) {
                files.emplace_back();
            } else {
                files.emplace_back(unpickleFile(p));
            }
        }
    }

    {
        Timer timeit(result.tracer(), "readNames");

        int namesSize = p.getU4();
        ENFORCE(namesSize > 0);
        names.reserve(nearestPowerOf2(namesSize));
        for (int i = 0; i < namesSize; i++) {
            if (i == 0) {
                auto &inserted = names.emplace_back();
                inserted.kind = NameKind::UTF8;
                inserted.raw.utf8 = string_view();
            } else {
                names.emplace_back(unpickleName(p, result));
            }
        }
    }

    {
        Timer timeit(result.tracer(), "readSymbols");

        int symbolSize = p.getU4();
        ENFORCE(symbolSize > 0);
        symbols.reserve(symbolSize);
        for (int i = 0; i < symbolSize; i++) {
            symbols.emplace_back(unpickleSymbol(p, &result));
        }
    }

    {
        Timer timeit(result.tracer(), "readNameTable");
        int namesByHashSize = p.getU4();
        names.reserve(namesByHashSize / 2);
        namesByHash.reserve(names.capacity() * 2);
        for (int i = 0; i < namesByHashSize; i++) {
            auto hash = p.getU4();
            auto value = p.getU4();
            namesByHash.emplace_back(make_pair(hash, value));
        }
    }

    UnorderedMap<string, FileRef> fileRefByPath;
    int i = 0;
    for (auto f : files) {
        if (f && !f->path().empty()) {
            fileRefByPath[string(f->path())] = FileRef(i);
        }
        i++;
    }

    {
        Timer timeit(result.tracer(), "moving");
        result.fileRefByPath = std::move(fileRefByPath);
        result.files = std::move(files);
        result.names = std::move(names);
        result.symbols = std::move(symbols);
        result.namesByHash = std::move(namesByHash);
    }
    result.sanityCheck();
}

void SerializerImpl::pickle(Pickler &p, LocOffsets loc) {
    p.putU4(loc.beginLoc);
    p.putU4(loc.endLoc);
}

void SerializerImpl::pickleWithFile(Pickler &p, Loc loc) {
    p.putU4(loc.file().id());
    pickle(p, loc.storage.offsets);
}
void SerializerImpl::pickleWithoutFile(Pickler &p, Loc loc) {
    pickle(p, loc.storage.offsets);
}

Loc SerializerImpl::unpickleLoc(UnPickler &p) {
    FileRef file(p.getU4());
    auto offsets = unpickleLocOffsets(p);
    return Loc(file, offsets);
}

Loc SerializerImpl::unpickleLocInFile(UnPickler &p, FileRef file) {
    Loc loc;
    LocOffsets offsets = unpickleLocOffsets(p);
    loc.setFile(file);
    loc.storage.offsets = offsets;
    return loc;
}

LocOffsets SerializerImpl::unpickleLocOffsets(UnPickler &p) {
    return LocOffsets{p.getU4(), p.getU4()};
}

vector<u1> Serializer::store(GlobalState &gs) {
    Pickler p = SerializerImpl::pickle(gs);
    return p.result(GLOBAL_STATE_COMPRESSION_DEGREE);
}

std::vector<u1> Serializer::storePayloadAndNameTable(GlobalState &gs) {
    Timer timeit(gs.tracer(), "Serializer::storePayloadAndNameTable");
    Pickler p = SerializerImpl::pickle(gs, true);
    return p.result(GLOBAL_STATE_COMPRESSION_DEGREE);
}

void Serializer::loadGlobalState(GlobalState &gs, const u1 *const data) {
    ENFORCE(gs.files.empty() && gs.names.empty() && gs.symbols.empty(), "Can't load into a non-empty state");
    UnPickler p(data, gs.tracer());
    SerializerImpl::unpickleGS(p, gs);
    gs.installIntrinsics();
}

u4 Serializer::loadGlobalStateUUID(const GlobalState &gs, const u1 *const data) {
    UnPickler p(data, gs.tracer());
    return SerializerImpl::unpickleGSUUID(p);
}

vector<u1> Serializer::storeFile(const core::File &file, ast::ParsedFile &tree) {
    Pickler p;
    SerializerImpl::pickle(p, file);
    SerializerImpl::pickleTree(p, tree.tree);
    return p.result(FILE_COMPRESSION_DEGREE);
}

CachedFile Serializer::loadFile(const core::GlobalState &gs, core::FileRef fref, const u1 *const data) {
    UnPickler p(data, gs.tracer());
    auto file = SerializerImpl::unpickleFile(p);
    file->cached = true;
    auto tree = SerializerImpl::unpickleExpr(p, gs, fref);
    return CachedFile{move(file), move(tree)};
}

void SerializerImpl::pickleTree(Pickler &p, ast::TreePtr &t) {
    pickle(p, t);
}

void SerializerImpl::pickleAstHeader(Pickler &p, u1 tag, ast::Expression *tree) {
    p.putU1(tag);
    pickle(p, tree->loc);
}

void SerializerImpl::pickle(Pickler &p, const ast::TreePtr &what) {
    if (what == nullptr) {
        p.putU1(1);
        return;
    }

    typecase(
        what.get(),
        [&](ast::Send *s) {
            pickleAstHeader(p, 2, s);
            p.putU4(s->fun._id);
            u1 flags;
            static_assert(sizeof(flags) == sizeof(s->flags));
            // Can replace this with std::bit_cast in C++20
            memcpy(&flags, &s->flags, sizeof(flags));
            p.putU1(flags);
            p.putU4(s->args.size());
            pickle(p, s->recv);
            pickleTree(p, s->block);
            for (auto &arg : s->args) {
                pickle(p, arg);
            }
        },
        [&](ast::Block *a) {
            pickleAstHeader(p, 3, a);
            p.putU4(a->args.size());
            pickle(p, a->body);
            for (auto &arg : a->args) {
                pickle(p, arg);
            };
        },
        [&](ast::Literal *a) {
            pickleAstHeader(p, 4, a);
            pickle(p, a->value.get());
        },
        [&](ast::While *a) {
            pickleAstHeader(p, 5, a);
            pickle(p, a->cond);
            pickle(p, a->body);
        },
        [&](ast::Return *a) {
            pickleAstHeader(p, 6, a);
            pickle(p, a->expr);
        },
        [&](ast::If *a) {
            pickleAstHeader(p, 7, a);
            pickle(p, a->cond);
            pickle(p, a->thenp);
            pickle(p, a->elsep);
        },

        [&](ast::UnresolvedConstantLit *a) {
            pickleAstHeader(p, 8, a);
            p.putU4(a->cnst._id);
            pickle(p, a->scope);
        },
        [&](ast::Local *a) {
            pickleAstHeader(p, 10, a);
            p.putU4(a->localVariable._name._id);
            p.putU4(a->localVariable.unique);
        },
        [&](ast::Assign *a) {
            pickleAstHeader(p, 12, a);
            pickle(p, a->lhs);
            pickle(p, a->rhs);
        },
        [&](ast::InsSeq *a) {
            pickleAstHeader(p, 13, a);
            p.putU4(a->stats.size());
            pickle(p, a->expr);
            for (auto &st : a->stats) {
                pickle(p, st);
            }
        },

        [&](ast::Next *a) {
            pickleAstHeader(p, 14, a);
            pickle(p, a->expr);
        },

        [&](ast::Break *a) {
            pickleAstHeader(p, 15, a);
            pickle(p, a->expr);
        },

        [&](ast::Retry *a) { pickleAstHeader(p, 16, a); },

        [&](ast::Hash *h) {
            pickleAstHeader(p, 17, h);
            ENFORCE(h->values.size() == h->keys.size());
            p.putU4(h->values.size());
            for (auto &v : h->values) {
                pickle(p, v);
            }
            for (auto &k : h->keys) {
                pickle(p, k);
            }
        },

        [&](ast::Array *a) {
            pickleAstHeader(p, 18, a);
            p.putU4(a->elems.size());
            for (auto &e : a->elems) {
                pickle(p, e);
            }
        },

        [&](ast::Cast *c) {
            pickleAstHeader(p, 19, c);
            p.putU4(c->cast._id);
            pickle(p, c->type.get());
            pickle(p, c->arg);
        },

        [&](ast::EmptyTree *n) { pickleAstHeader(p, 20, n); },
        [&](ast::ClassDef *c) {
            pickleAstHeader(p, 21, c);
            pickleWithoutFile(p, c->declLoc);
            p.putU1(static_cast<u2>(c->kind));
            p.putU4(c->symbol._id);
            p.putU4(c->ancestors.size());
            p.putU4(c->singletonAncestors.size());
            p.putU4(c->rhs.size());
            pickle(p, c->name);
            for (auto &anc : c->ancestors) {
                pickle(p, anc);
            }
            for (auto &anc : c->singletonAncestors) {
                pickle(p, anc);
            }
            for (auto &anc : c->rhs) {
                pickle(p, anc);
            }
        },
        [&](ast::MethodDef *c) {
            pickleAstHeader(p, 22, c);
            pickleWithoutFile(p, c->declLoc);
            u1 flags;
            static_assert(sizeof(flags) == sizeof(c->flags));
            // Can replace this with std::bit_cast in C++20
            memcpy(&flags, &c->flags, sizeof(flags));
            p.putU1(flags);
            p.putU4(c->name._id);
            p.putU4(c->symbol._id);
            p.putU4(c->args.size());
            pickle(p, c->rhs);
            for (auto &a : c->args) {
                pickle(p, a);
            }
        },
        [&](ast::Rescue *a) {
            pickleAstHeader(p, 23, a);
            p.putU4(a->rescueCases.size());
            pickle(p, a->ensure);
            pickle(p, a->else_);
            pickle(p, a->body);
            for (auto &rc : a->rescueCases) {
                pickleTree(p, rc);
            }
        },
        [&](ast::RescueCase *a) {
            pickleAstHeader(p, 24, a);
            p.putU4(a->exceptions.size());
            pickle(p, a->var);
            pickle(p, a->body);
            for (auto &ex : a->exceptions) {
                pickle(p, ex);
            }
        },
        [&](ast::RestArg *a) {
            pickleAstHeader(p, 25, a);
            pickleTree(p, a->expr);
        },
        [&](ast::KeywordArg *a) {
            pickleAstHeader(p, 26, a);
            pickleTree(p, a->expr);
        },
        [&](ast::ShadowArg *a) {
            pickleAstHeader(p, 27, a);
            pickleTree(p, a->expr);
        },
        [&](ast::BlockArg *a) {
            pickleAstHeader(p, 28, a);
            pickleTree(p, a->expr);
        },
        [&](ast::OptionalArg *a) {
            pickleAstHeader(p, 29, a);
            pickleTree(p, a->expr);
            pickle(p, a->default_);
        },
        [&](ast::ZSuperArgs *a) { pickleAstHeader(p, 30, a); },
        [&](ast::UnresolvedIdent *a) {
            pickleAstHeader(p, 31, a);
            p.putU1(static_cast<u1>(a->kind));
            p.putU4(a->name._id);
        },
        [&](ast::ConstantLit *a) {
            pickleAstHeader(p, 32, a);
            p.putU4(a->symbol._id);
            pickleTree(p, a->original);
        },

        [&](ast::Expression *n) { Exception::raise("Unimplemented AST Node: {}", n->nodeName()); });
}

ast::TreePtr SerializerImpl::unpickleExpr(serialize::UnPickler &p, const GlobalState &gs, FileRef file) {
    u1 kind = p.getU1();
    if (kind == 1) {
        return nullptr;
    }
    LocOffsets loc = unpickleLocOffsets(p);

    switch (kind) {
        case 2: {
            NameRef fun = unpickleNameRef(p, gs);
            auto flagsU1 = p.getU1();
            ast::Send::Flags flags;
            static_assert(sizeof(flags) == sizeof(flagsU1));
            // Can replace this with std::bit_cast in C++20
            memcpy(&flags, &flagsU1, sizeof(flags));
            auto argsSize = p.getU4();
            auto recv = unpickleExpr(p, gs, file);
            auto blkt = unpickleExpr(p, gs, file);
            ast::TreePtr blk;
            if (blkt) {
                blk.reset(static_cast<ast::Block *>(blkt.release()));
            }
            ast::Send::ARGS_store store(argsSize);
            for (auto &expr : store) {
                expr = unpickleExpr(p, gs, file);
            }
            return ast::MK::Send(loc, std::move(recv), fun, std::move(store), flags, std::move(blk));
        }
        case 3: {
            auto argsSize = p.getU4();
            auto body = unpickleExpr(p, gs, file);
            ast::MethodDef::ARGS_store args(argsSize);
            for (auto &arg : args) {
                arg = unpickleExpr(p, gs, file);
            }
            return ast::MK::Block(loc, std::move(body), std::move(args));
        }
        case 4: {
            auto tpe = unpickleType(p, &gs);
            return ast::MK::Literal(loc, tpe);
        }
        case 5: {
            auto cond = unpickleExpr(p, gs, file);
            auto body = unpickleExpr(p, gs, file);
            return ast::MK::While(loc, std::move(cond), std::move(body));
        }
        case 6: {
            auto expr = unpickleExpr(p, gs, file);
            return ast::MK::Return(loc, std::move(expr));
        }
        case 7: {
            auto cond = unpickleExpr(p, gs, file);
            auto thenp = unpickleExpr(p, gs, file);
            auto elsep = unpickleExpr(p, gs, file);
            return ast::MK::If(loc, std::move(cond), std::move(thenp), std::move(elsep));
        }
        case 8: {
            NameRef cnst = unpickleNameRef(p, gs);
            auto scope = unpickleExpr(p, gs, file);
            return ast::MK::UnresolvedConstant(loc, std::move(scope), cnst);
        }
        case 10: {
            NameRef nm = unpickleNameRef(p, gs);
            auto unique = p.getU4();
            LocalVariable lv(nm, unique);
            return ast::make_tree<ast::Local>(loc, lv);
        }
        case 12: {
            auto lhs = unpickleExpr(p, gs, file);
            auto rhs = unpickleExpr(p, gs, file);
            return ast::MK::Assign(loc, std::move(lhs), std::move(rhs));
        }
        case 13: {
            auto insSize = p.getU4();
            auto expr = unpickleExpr(p, gs, file);
            ast::InsSeq::STATS_store stats(insSize);
            for (auto &stat : stats) {
                stat = unpickleExpr(p, gs, file);
            }
            return ast::MK::InsSeq(loc, std::move(stats), std::move(expr));
        }
        case 14: {
            auto expr = unpickleExpr(p, gs, file);
            return ast::MK::Next(loc, std::move(expr));
        }
        case 15: {
            auto expr = unpickleExpr(p, gs, file);
            return ast::MK::Break(loc, std::move(expr));
        }
        case 16: {
            return ast::make_tree<ast::Retry>(loc);
        }
        case 17: {
            auto sz = p.getU4();
            ast::Hash::ENTRY_store keys(sz);
            ast::Hash::ENTRY_store values(sz);
            for (auto &value : values) {
                value = unpickleExpr(p, gs, file);
            }
            for (auto &key : keys) {
                key = unpickleExpr(p, gs, file);
            }
            return ast::MK::Hash(loc, std::move(keys), std::move(values));
        }
        case 18: {
            auto sz = p.getU4();
            ast::Array::ENTRY_store elems(sz);
            for (auto &elem : elems) {
                elem = unpickleExpr(p, gs, file);
            }
            return ast::MK::Array(loc, std::move(elems));
        }
        case 19: {
            NameRef kind(gs, p.getU4());
            auto type = unpickleType(p, &gs);
            auto arg = unpickleExpr(p, gs, file);
            return ast::make_tree<ast::Cast>(loc, std::move(type), std::move(arg), kind);
        }
        case 20: {
            return ast::MK::EmptyTree();
        }
        case 21: {
            auto declLoc = unpickleLocInFile(p, file);
            auto kind = p.getU1();
            SymbolRef symbol(gs, p.getU4());
            auto ancestorsSize = p.getU4();
            auto singletonAncestorsSize = p.getU4();
            auto rhsSize = p.getU4();
            auto name = unpickleExpr(p, gs, file);
            ast::ClassDef::ANCESTORS_store ancestors(ancestorsSize);
            for (auto &anc : ancestors) {
                anc = unpickleExpr(p, gs, file);
            }
            ast::ClassDef::ANCESTORS_store singletonAncestors(singletonAncestorsSize);
            for (auto &sanc : singletonAncestors) {
                sanc = unpickleExpr(p, gs, file);
            }
            ast::ClassDef::RHS_store rhs(rhsSize);
            for (auto &r : rhs) {
                r = unpickleExpr(p, gs, file);
            }
            auto ret = ast::MK::ClassOrModule(loc, declLoc, std::move(name), std::move(ancestors), std::move(rhs),
                                              (ast::ClassDef::Kind)kind);
            {
                auto klass = ast::cast_tree<ast::ClassDef>(ret);
                klass->singletonAncestors = std::move(singletonAncestors);
                klass->symbol = symbol;
            }
            return ret;
        }
        case 22: {
            auto declLoc = unpickleLocInFile(p, file);
            auto flagsU1 = p.getU1();
            ast::MethodDef::Flags flags;
            static_assert(sizeof(flags) == sizeof(flagsU1));
            // Can replace this with std::bit_cast in C++20
            memcpy(&flags, &flagsU1, sizeof(flags));
            NameRef name = unpickleNameRef(p, gs);
            SymbolRef symbol(gs, p.getU4());
            auto argsSize = p.getU4();
            auto rhs = unpickleExpr(p, gs, file);
            ast::MethodDef::ARGS_store args(argsSize);
            for (auto &arg : args) {
                arg = unpickleExpr(p, gs, file);
            }
            auto ret = ast::MK::SyntheticMethod(loc, declLoc, name, std::move(args), std::move(rhs));

            {
                auto *method = ast::cast_tree<ast::MethodDef>(ret);
                method->flags = flags;
                method->symbol = symbol;
            }
            return ret;
        }
        case 23: {
            auto rescueCasesSize = p.getU4();
            auto ensure = unpickleExpr(p, gs, file);
            auto else_ = unpickleExpr(p, gs, file);
            auto body_ = unpickleExpr(p, gs, file);
            ast::Rescue::RESCUE_CASE_store cases(rescueCasesSize);
            for (auto &case_ : cases) {
                auto t = unpickleExpr(p, gs, file);
                case_.reset(static_cast<ast::RescueCase *>(t.release()));
            }
            return ast::make_tree<ast::Rescue>(loc, std::move(body_), std::move(cases), std::move(else_),
                                               std::move(ensure));
        }
        case 24: {
            auto exceptionsSize = p.getU4();
            auto var = unpickleExpr(p, gs, file);
            auto body = unpickleExpr(p, gs, file);
            ast::RescueCase::EXCEPTION_store exceptions(exceptionsSize);
            for (auto &ex : exceptions) {
                ex = unpickleExpr(p, gs, file);
            }
            return ast::make_tree<ast::RescueCase>(loc, std::move(exceptions), std::move(var), std::move(body));
        }
        case 25: {
            auto ref = unpickleExpr(p, gs, file);
            return ast::make_tree<ast::RestArg>(loc, std::move(ref));
        }
        case 26: {
            auto ref = unpickleExpr(p, gs, file);
            return ast::make_tree<ast::KeywordArg>(loc, std::move(ref));
        }
        case 27: {
            auto ref = unpickleExpr(p, gs, file);
            return ast::make_tree<ast::ShadowArg>(loc, std::move(ref));
        }
        case 28: {
            auto ref = unpickleExpr(p, gs, file);
            return ast::make_tree<ast::BlockArg>(loc, std::move(ref));
        }
        case 29: {
            auto ref = unpickleExpr(p, gs, file);
            auto default_ = unpickleExpr(p, gs, file);
            return ast::MK::OptionalArg(loc, std::move(ref), std::move(default_));
        }
        case 30: {
            return ast::make_tree<ast::ZSuperArgs>(loc);
        }
        case 31: {
            auto kind = (ast::UnresolvedIdent::Kind)p.getU1();
            NameRef name = unpickleNameRef(p, gs);
            return ast::make_tree<ast::UnresolvedIdent>(loc, kind, name);
        }
        case 32: {
            SymbolRef sym(gs, p.getU4());
            auto orig = unpickleExpr(p, gs, file);
            return ast::make_tree<ast::ConstantLit>(loc, sym, std::move(orig));
        }
    }
    Exception::raise("Not handled {}", kind);
}

NameRef SerializerImpl::unpickleNameRef(UnPickler &p, const GlobalState &gs) {
    NameRef name(NameRef::WellKnown{}, p.getU4());
    ENFORCE(name.data(gs)->ref(gs) == name);
    return name;
}

} // namespace sorbet::core::serialize
