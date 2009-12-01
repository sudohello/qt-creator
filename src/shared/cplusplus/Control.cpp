/**************************************************************************
**
** This file is part of Qt Creator
**
** Copyright (c) 2009 Nokia Corporation and/or its subsidiary(-ies).
**
** Contact: Nokia Corporation (qt-info@nokia.com)
**
** Commercial Usage
**
** Licensees holding valid Qt Commercial licenses may use this file in
** accordance with the Qt Commercial License Agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and Nokia.
**
** GNU Lesser General Public License Usage
**
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 2.1 as published by the Free Software
** Foundation and appearing in the file LICENSE.LGPL included in the
** packaging of this file.  Please review the following information to
** ensure the GNU Lesser General Public License version 2.1 requirements
** will be met: http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html.
**
** If you are unsure which license is appropriate for your use, please
** contact the sales department at http://qt.nokia.com/contact.
**
**************************************************************************/
// Copyright (c) 2008 Roberto Raggi <roberto.raggi@gmail.com>
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

#include "Control.h"
#include "Literals.h"
#include "LiteralTable.h"
#include "TranslationUnit.h"
#include "CoreTypes.h"
#include "Symbols.h"
#include "Names.h"
#include "Array.h"
#include "TypeMatcher.h"
#include <map>
#include <set>

using namespace CPlusPlus;

namespace {

template <typename _Tp>
struct Compare;

template <> struct Compare<IntegerType>
{
    bool operator()(const IntegerType &ty, const IntegerType &otherTy) const
    { return ty.kind() < otherTy.kind(); }
};

template <> struct Compare<FloatType>
{
    bool operator()(const FloatType &ty, const FloatType &otherTy) const
    { return ty.kind() < otherTy.kind(); }
};

template <> struct Compare<PointerToMemberType>
{
    bool operator()(const PointerToMemberType &ty, const PointerToMemberType &otherTy) const
    {
        if (ty.memberName() < otherTy.memberName())
            return true;

        else if (ty.memberName() == otherTy.memberName())
            return ty.elementType() < otherTy.elementType();

        return false;
    }
};

template <> struct Compare<PointerType>
{
    bool operator()(const PointerType &ty, const PointerType &otherTy) const
    {
        return ty.elementType() < otherTy.elementType();
    }
};

template <> struct Compare<ReferenceType>
{
    bool operator()(const ReferenceType &ty, const ReferenceType &otherTy) const
    {
        return ty.elementType() < otherTy.elementType();
    }
};

template <> struct Compare<NamedType>
{
    bool operator()(const NamedType &ty, const NamedType &otherTy) const
    {
        return ty.name() < otherTy.name();
    }
};

template <> struct Compare<ArrayType>
{
    bool operator()(const ArrayType &ty, const ArrayType &otherTy) const
    {
        if (ty.size() < otherTy.size())
            return true;

        else if (ty.size() == otherTy.size())
            return ty.elementType() < otherTy.elementType();

        return false;
    }
};

template <typename _Tp>
class Table: public std::set<_Tp, Compare<_Tp> >
{
public:
    _Tp *intern(const _Tp &element)
    { return const_cast<_Tp *>(&*insert(element).first); }
};

} // end of anonymous namespace

template <typename _Iterator>
static void delete_map_entries(_Iterator first, _Iterator last)
{
    for (; first != last; ++first)
        delete first->second;
}

template <typename _Iterator>
static void delete_array_entries(_Iterator first, _Iterator last)
{
    for (; first != last; ++first)
        delete *first;
}

template <typename _Map>
static void delete_map_entries(const _Map &m)
{ delete_map_entries(m.begin(), m.end()); }

template <typename _Array>
static void delete_array_entries(const _Array &a)
{ delete_array_entries(a.begin(), a.end()); }

class Control::Data
{
public:
    Data(Control *control)
        : control(control),
          translationUnit(0),
          diagnosticClient(0)
    {}

    ~Data()
    {
        // names
        delete_map_entries(nameIds);
        delete_map_entries(destructorNameIds);
        delete_map_entries(operatorNameIds);
        delete_map_entries(conversionNameIds);
        delete_map_entries(qualifiedNameIds);
        delete_map_entries(templateNameIds);

        // symbols
        delete_array_entries(symbols);
    }

    NameId *findOrInsertNameId(const Identifier *id)
    {
        if (! id)
            return 0;
        std::map<const Identifier *, NameId *>::iterator it = nameIds.lower_bound(id);
        if (it == nameIds.end() || it->first != id)
            it = nameIds.insert(it, std::make_pair(id, new NameId(id)));
        return it->second;
    }

    TemplateNameId *findOrInsertTemplateNameId(const Identifier *id,
        const std::vector<FullySpecifiedType> &templateArguments)
    {
        if (! id)
            return 0;
        const TemplateNameIdKey key(id, templateArguments);
        std::map<TemplateNameIdKey, TemplateNameId *>::iterator it =
                templateNameIds.lower_bound(key);
        if (it == templateNameIds.end() || it->first != key) {
            const FullySpecifiedType *args = 0;
            if (templateArguments.size())
                args = &templateArguments[0];
            TemplateNameId *templ = new TemplateNameId(id, args,
                                                       templateArguments.size());
            it = templateNameIds.insert(it, std::make_pair(key, templ));
        }
        return it->second;
    }

    DestructorNameId *findOrInsertDestructorNameId(const Identifier *id)
    {
        if (! id)
            return 0;
        std::map<const Identifier *, DestructorNameId *>::iterator it = destructorNameIds.lower_bound(id);
        if (it == destructorNameIds.end() || it->first != id)
            it = destructorNameIds.insert(it, std::make_pair(id, new DestructorNameId(id)));
        return it->second;
    }

    OperatorNameId *findOrInsertOperatorNameId(int kind)
    {
        const int key(kind);
        std::map<int, OperatorNameId *>::iterator it = operatorNameIds.lower_bound(key);
        if (it == operatorNameIds.end() || it->first != key)
            it = operatorNameIds.insert(it, std::make_pair(key, new OperatorNameId(kind)));
        return it->second;
    }

    ConversionNameId *findOrInsertConversionNameId(const FullySpecifiedType &type)
    {
        std::map<FullySpecifiedType, ConversionNameId *>::iterator it =
                conversionNameIds.lower_bound(type);
        if (it == conversionNameIds.end() || it->first != type)
            it = conversionNameIds.insert(it, std::make_pair(type, new ConversionNameId(type)));
        return it->second;
    }

    QualifiedNameId *findOrInsertQualifiedNameId(const std::vector<Name *> &names, bool isGlobal)
    {
        const QualifiedNameIdKey key(names, isGlobal);
        std::map<QualifiedNameIdKey, QualifiedNameId *>::iterator it =
                qualifiedNameIds.lower_bound(key);
        if (it == qualifiedNameIds.end() || it->first != key) {
            QualifiedNameId *name = new QualifiedNameId(&names[0], names.size(), isGlobal);
            it = qualifiedNameIds.insert(it, std::make_pair(key, name));
        }
        return it->second;
    }

    SelectorNameId *findOrInsertSelectorNameId(const std::vector<Name *> &names, bool hasArguments)
    {
        const SelectorNameIdKey key(names, hasArguments);
        std::map<SelectorNameIdKey, SelectorNameId *>::iterator it = selectorNameIds.lower_bound(key);
        if (it == selectorNameIds.end() || it->first != key)
            it = selectorNameIds.insert(it, std::make_pair(key, new SelectorNameId(&names[0], names.size(), hasArguments)));
        return it->second;
    }

    IntegerType *findOrInsertIntegerType(int kind)
    {
        return integerTypes.intern(IntegerType(kind));
    }

    FloatType *findOrInsertFloatType(int kind)
    {
        return floatTypes.intern(FloatType(kind));
    }

    PointerToMemberType *findOrInsertPointerToMemberType(Name *memberName, const FullySpecifiedType &elementType)
    {
        return pointerToMemberTypes.intern(PointerToMemberType(memberName, elementType));
    }

    PointerType *findOrInsertPointerType(const FullySpecifiedType &elementType)
    {
        return pointerTypes.intern(PointerType(elementType));
    }

    ReferenceType *findOrInsertReferenceType(const FullySpecifiedType &elementType)
    {
        return referenceTypes.intern(ReferenceType(elementType));
    }

    ArrayType *findOrInsertArrayType(const FullySpecifiedType &elementType, unsigned size)
    {
        return arrayTypes.intern(ArrayType(elementType, size));
    }

    NamedType *findOrInsertNamedType(Name *name)
    {
        return namedTypes.intern(NamedType(name));
    }

    Declaration *newDeclaration(unsigned sourceLocation, Name *name)
    {
        Declaration *declaration = new Declaration(translationUnit,
                                                   sourceLocation, name);
        symbols.push_back(declaration);
        return declaration;
    }

    Argument *newArgument(unsigned sourceLocation, Name *name)
    {
        Argument *argument = new Argument(translationUnit,
                                          sourceLocation, name);
        symbols.push_back(argument);
        return argument;
    }

    Function *newFunction(unsigned sourceLocation, Name *name)
    {
        Function *function = new Function(translationUnit,
                                          sourceLocation, name);
        symbols.push_back(function);
        return function;
    }

    BaseClass *newBaseClass(unsigned sourceLocation, Name *name)
    {
        BaseClass *baseClass = new BaseClass(translationUnit,
                                             sourceLocation, name);
        symbols.push_back(baseClass);
        return baseClass;
    }

    Block *newBlock(unsigned sourceLocation)
    {
        Block *block = new Block(translationUnit, sourceLocation);
        symbols.push_back(block);
        return block;
    }

    Class *newClass(unsigned sourceLocation, Name *name)
    {
        Class *klass = new Class(translationUnit,
                                 sourceLocation, name);
        symbols.push_back(klass);
        return klass;
    }

    Namespace *newNamespace(unsigned sourceLocation, Name *name)
    {
        Namespace *ns = new Namespace(translationUnit,
                                      sourceLocation, name);
        symbols.push_back(ns);
        return ns;
    }

    UsingNamespaceDirective *newUsingNamespaceDirective(unsigned sourceLocation, Name *name)
    {
        UsingNamespaceDirective *u = new UsingNamespaceDirective(translationUnit,
                                                                 sourceLocation, name);
        symbols.push_back(u);
        return u;
    }

    ForwardClassDeclaration *newForwardClassDeclaration(unsigned sourceLocation, Name *name)
    {
        ForwardClassDeclaration *c = new ForwardClassDeclaration(translationUnit,
                                                                 sourceLocation, name);
        symbols.push_back(c);
        return c;
    }

    ObjCBaseClass *newObjCBaseClass(unsigned sourceLocation, Name *name)
    {
        ObjCBaseClass *c = new ObjCBaseClass(translationUnit, sourceLocation, name);
        symbols.push_back(c);
        return c;
    }

    ObjCBaseProtocol *newObjCBaseProtocol(unsigned sourceLocation, Name *name)
    {
        ObjCBaseProtocol *p = new ObjCBaseProtocol(translationUnit, sourceLocation, name);
        symbols.push_back(p);
        return p;
    }

    ObjCClass *newObjCClass(unsigned sourceLocation, Name *name)
    {
        ObjCClass *c = new ObjCClass(translationUnit, sourceLocation, name);
        symbols.push_back(c);
        return c;
    }

    ObjCForwardClassDeclaration *newObjCForwardClassDeclaration(unsigned sourceLocation, Name *name)
    {
        ObjCForwardClassDeclaration *fwd = new ObjCForwardClassDeclaration(translationUnit, sourceLocation, name);
        symbols.push_back(fwd);
        return fwd;
    }

    ObjCProtocol *newObjCProtocol(unsigned sourceLocation, Name *name)
    {
        ObjCProtocol *p = new ObjCProtocol(translationUnit, sourceLocation, name);
        symbols.push_back(p);
        return p;
    }

    ObjCForwardProtocolDeclaration *newObjCForwardProtocolDeclaration(unsigned sourceLocation, Name *name)
    {
        ObjCForwardProtocolDeclaration *fwd = new ObjCForwardProtocolDeclaration(translationUnit, sourceLocation, name);
        symbols.push_back(fwd);
        return fwd;
    }

    ObjCMethod *newObjCMethod(unsigned sourceLocation, Name *name)
    {
        ObjCMethod *method = new ObjCMethod(translationUnit, sourceLocation, name);
        symbols.push_back(method);
        return method;
    }

    ObjCPropertyDeclaration *newObjCPropertyDeclaration(unsigned sourceLocation, Name *name)
    {
        ObjCPropertyDeclaration *decl = new ObjCPropertyDeclaration(translationUnit, sourceLocation, name);
        symbols.push_back(decl);
        return decl;
    }

    Enum *newEnum(unsigned sourceLocation, Name *name)
    {
        Enum *e = new Enum(translationUnit,
                           sourceLocation, name);
        symbols.push_back(e);
        return e;
    }

    UsingDeclaration *newUsingDeclaration(unsigned sourceLocation, Name *name)
    {
        UsingDeclaration *u = new UsingDeclaration(translationUnit,
                                                   sourceLocation, name);
        symbols.push_back(u);
        return u;
    }

    struct TemplateNameIdKey {
        const Identifier *id;
        std::vector<FullySpecifiedType> templateArguments;

        TemplateNameIdKey(const Identifier *id, const std::vector<FullySpecifiedType> &templateArguments)
            : id(id), templateArguments(templateArguments)
        { }

        bool operator == (const TemplateNameIdKey &other) const
        { return id == other.id && templateArguments == other.templateArguments; }

        bool operator != (const TemplateNameIdKey &other) const
        { return ! operator==(other); }

        bool operator < (const TemplateNameIdKey &other) const
        {
            if (id == other.id)
                return std::lexicographical_compare(templateArguments.begin(),
                                                    templateArguments.end(),
                                                    other.templateArguments.begin(),
                                                    other.templateArguments.end());
            return id < other.id;
        }
    };

    struct QualifiedNameIdKey {
        std::vector<Name *> names;
        bool isGlobal;

        QualifiedNameIdKey(const std::vector<Name *> &names, bool isGlobal) :
            names(names), isGlobal(isGlobal)
        { }

        bool operator == (const QualifiedNameIdKey &other) const
        { return isGlobal == other.isGlobal && names == other.names; }

        bool operator != (const QualifiedNameIdKey &other) const
        { return ! operator==(other); }

        bool operator < (const QualifiedNameIdKey &other) const
        {
            if (isGlobal == other.isGlobal)
                return std::lexicographical_compare(names.begin(), names.end(),
                                                    other.names.begin(), other.names.end());
            return isGlobal < other.isGlobal;
        }
    };

    struct SelectorNameIdKey {
        std::vector<Name *> _names;
        bool _hasArguments;

        SelectorNameIdKey(const std::vector<Name *> &names, bool hasArguments): _names(names), _hasArguments(hasArguments) {}

        bool operator==(const SelectorNameIdKey &other) const
        { return _names == other._names && _hasArguments == other._hasArguments; }

        bool operator!=(const SelectorNameIdKey &other) const
        { return !operator==(other); }

        bool operator<(const SelectorNameIdKey &other) const
        {
            if (_hasArguments == other._hasArguments)
                return std::lexicographical_compare(_names.begin(), _names.end(), other._names.begin(), other._names.end());
            else
                return _hasArguments < other._hasArguments;
        }
    };

    Control *control;
    TranslationUnit *translationUnit;
    DiagnosticClient *diagnosticClient;

    TypeMatcher matcher;

    LiteralTable<Identifier> identifiers;
    LiteralTable<StringLiteral> stringLiterals;
    LiteralTable<NumericLiteral> numericLiterals;

    // ### replace std::map with lookup tables. ASAP!

    // names
    std::map<const Identifier *, NameId *> nameIds;
    std::map<const Identifier *, DestructorNameId *> destructorNameIds;
    std::map<int, OperatorNameId *> operatorNameIds;
    std::map<FullySpecifiedType, ConversionNameId *> conversionNameIds;
    std::map<TemplateNameIdKey, TemplateNameId *> templateNameIds;
    std::map<QualifiedNameIdKey, QualifiedNameId *> qualifiedNameIds;
    std::map<SelectorNameIdKey, SelectorNameId *> selectorNameIds;

    // types
    VoidType voidType;
    Table<IntegerType> integerTypes;
    Table<FloatType> floatTypes;
    Table<PointerToMemberType> pointerToMemberTypes;
    Table<PointerType> pointerTypes;
    Table<ReferenceType> referenceTypes;
    Table<ArrayType> arrayTypes;
    Table<NamedType> namedTypes;

    // symbols
    std::vector<Symbol *> symbols;

    // ObjC context keywords:
    const Identifier *objcGetterId;
    const Identifier *objcSetterId;
    const Identifier *objcReadwriteId;
    const Identifier *objcReadonlyId;
    const Identifier *objcAssignId;
    const Identifier *objcRetainId;
    const Identifier *objcCopyId;
    const Identifier *objcNonatomicId;
};

Control::Control()
{
    d = new Data(this);

    d->objcGetterId = findOrInsertIdentifier("getter");
    d->objcSetterId = findOrInsertIdentifier("setter");
    d->objcReadwriteId = findOrInsertIdentifier("readwrite");
    d->objcReadonlyId = findOrInsertIdentifier("readonly");
    d->objcAssignId = findOrInsertIdentifier("assign");
    d->objcRetainId = findOrInsertIdentifier("retain");
    d->objcCopyId = findOrInsertIdentifier("copy");
    d->objcNonatomicId = findOrInsertIdentifier("nonatomic");
}

Control::~Control()
{ delete d; }

TranslationUnit *Control::translationUnit() const
{ return d->translationUnit; }

TranslationUnit *Control::switchTranslationUnit(TranslationUnit *unit)
{
    TranslationUnit *previousTranslationUnit = d->translationUnit;
    d->translationUnit = unit;
    return previousTranslationUnit;
}

DiagnosticClient *Control::diagnosticClient() const
{ return d->diagnosticClient; }

void Control::setDiagnosticClient(DiagnosticClient *diagnosticClient)
{ d->diagnosticClient = diagnosticClient; }

const Identifier *Control::findIdentifier(const char *chars, unsigned size) const
{ return d->identifiers.findLiteral(chars, size); }

const Identifier *Control::findOrInsertIdentifier(const char *chars, unsigned size)
{ return d->identifiers.findOrInsertLiteral(chars, size); }

const Identifier *Control::findOrInsertIdentifier(const char *chars)
{
    unsigned length = std::strlen(chars);
    return findOrInsertIdentifier(chars, length);
}

Control::IdentifierIterator Control::firstIdentifier() const
{ return d->identifiers.begin(); }

Control::IdentifierIterator Control::lastIdentifier() const
{ return d->identifiers.end(); }

Control::StringLiteralIterator Control::firstStringLiteral() const
{ return d->stringLiterals.begin(); }

Control::StringLiteralIterator Control::lastStringLiteral() const
{ return d->stringLiterals.end(); }

Control::NumericLiteralIterator Control::firstNumericLiteral() const
{ return d->numericLiterals.begin(); }

Control::NumericLiteralIterator Control::lastNumericLiteral() const
{ return d->numericLiterals.end(); }

const StringLiteral *Control::findOrInsertStringLiteral(const char *chars, unsigned size)
{ return d->stringLiterals.findOrInsertLiteral(chars, size); }

const StringLiteral *Control::findOrInsertStringLiteral(const char *chars)
{
    unsigned length = std::strlen(chars);
    return findOrInsertStringLiteral(chars, length);
}

const NumericLiteral *Control::findOrInsertNumericLiteral(const char *chars, unsigned size)
{ return d->numericLiterals.findOrInsertLiteral(chars, size); }

const NumericLiteral *Control::findOrInsertNumericLiteral(const char *chars)
{
    unsigned length = std::strlen(chars);
    return findOrInsertNumericLiteral(chars, length);
}

NameId *Control::nameId(const Identifier *id)
{ return d->findOrInsertNameId(id); }

TemplateNameId *Control::templateNameId(const Identifier *id,
                                        FullySpecifiedType *const args,
                                        unsigned argv)
{
    std::vector<FullySpecifiedType> templateArguments(args, args + argv);
    return d->findOrInsertTemplateNameId(id, templateArguments);
}

DestructorNameId *Control::destructorNameId(const Identifier *id)
{ return d->findOrInsertDestructorNameId(id); }

OperatorNameId *Control::operatorNameId(int kind)
{ return d->findOrInsertOperatorNameId(kind); }

ConversionNameId *Control::conversionNameId(const FullySpecifiedType &type)
{ return d->findOrInsertConversionNameId(type); }

QualifiedNameId *Control::qualifiedNameId(Name *const *names,
                                             unsigned nameCount,
                                             bool isGlobal)
{
    std::vector<Name *> classOrNamespaceNames(names, names + nameCount);
    return d->findOrInsertQualifiedNameId(classOrNamespaceNames, isGlobal);
}

SelectorNameId *Control::selectorNameId(Name *const *names,
                                        unsigned nameCount,
                                        bool hasArguments)
{
    std::vector<Name *> selectorNames(names, names + nameCount);
    return d->findOrInsertSelectorNameId(selectorNames, hasArguments);
}


VoidType *Control::voidType()
{ return &d->voidType; }

IntegerType *Control::integerType(int kind)
{ return d->findOrInsertIntegerType(kind); }

FloatType *Control::floatType(int kind)
{ return d->findOrInsertFloatType(kind); }

PointerToMemberType *Control::pointerToMemberType(Name *memberName, const FullySpecifiedType &elementType)
{ return d->findOrInsertPointerToMemberType(memberName, elementType); }

PointerType *Control::pointerType(const FullySpecifiedType &elementType)
{ return d->findOrInsertPointerType(elementType); }

ReferenceType *Control::referenceType(const FullySpecifiedType &elementType)
{ return d->findOrInsertReferenceType(elementType); }

ArrayType *Control::arrayType(const FullySpecifiedType &elementType, unsigned size)
{ return d->findOrInsertArrayType(elementType, size); }

NamedType *Control::namedType(Name *name)
{ return d->findOrInsertNamedType(name); }

Argument *Control::newArgument(unsigned sourceLocation, Name *name)
{ return d->newArgument(sourceLocation, name); }

Function *Control::newFunction(unsigned sourceLocation, Name *name)
{ return d->newFunction(sourceLocation, name); }

Namespace *Control::newNamespace(unsigned sourceLocation, Name *name)
{ return d->newNamespace(sourceLocation, name); }

BaseClass *Control::newBaseClass(unsigned sourceLocation, Name *name)
{ return d->newBaseClass(sourceLocation, name); }

Class *Control::newClass(unsigned sourceLocation, Name *name)
{ return d->newClass(sourceLocation, name); }

Enum *Control::newEnum(unsigned sourceLocation, Name *name)
{ return d->newEnum(sourceLocation, name); }

Block *Control::newBlock(unsigned sourceLocation)
{ return d->newBlock(sourceLocation); }

Declaration *Control::newDeclaration(unsigned sourceLocation, Name *name)
{ return d->newDeclaration(sourceLocation, name); }

UsingNamespaceDirective *Control::newUsingNamespaceDirective(unsigned sourceLocation,
                                                                Name *name)
{ return d->newUsingNamespaceDirective(sourceLocation, name); }

UsingDeclaration *Control::newUsingDeclaration(unsigned sourceLocation, Name *name)
{ return d->newUsingDeclaration(sourceLocation, name); }

ForwardClassDeclaration *Control::newForwardClassDeclaration(unsigned sourceLocation,
                                                             Name *name)
{ return d->newForwardClassDeclaration(sourceLocation, name); }

ObjCBaseClass *Control::newObjCBaseClass(unsigned sourceLocation, Name *name)
{ return d->newObjCBaseClass(sourceLocation, name); }

ObjCBaseProtocol *Control::newObjCBaseProtocol(unsigned sourceLocation, Name *name)
{ return d->newObjCBaseProtocol(sourceLocation, name); }

ObjCClass *Control::newObjCClass(unsigned sourceLocation, Name *name)
{ return d->newObjCClass(sourceLocation, name); }

ObjCForwardClassDeclaration *Control::newObjCForwardClassDeclaration(unsigned sourceLocation, Name *name)
{ return d->newObjCForwardClassDeclaration(sourceLocation, name); }

ObjCProtocol *Control::newObjCProtocol(unsigned sourceLocation, Name *name)
{ return d->newObjCProtocol(sourceLocation, name); }

ObjCForwardProtocolDeclaration *Control::newObjCForwardProtocolDeclaration(unsigned sourceLocation, Name *name)
{ return d->newObjCForwardProtocolDeclaration(sourceLocation, name); }

ObjCMethod *Control::newObjCMethod(unsigned sourceLocation, Name *name)
{ return d->newObjCMethod(sourceLocation, name); }

ObjCPropertyDeclaration *Control::newObjCPropertyDeclaration(unsigned sourceLocation, Name *name)
{ return d->newObjCPropertyDeclaration(sourceLocation, name); }

const Identifier *Control::objcGetterId() const
{ return d->objcGetterId; }

const Identifier *Control::objcSetterId() const
{ return d->objcSetterId; }

const Identifier *Control::objcReadwriteId() const
{ return d->objcReadwriteId; }

const Identifier *Control::objcReadonlyId() const
{ return d->objcReadonlyId; }

const Identifier *Control::objcAssignId() const
{ return d->objcAssignId; }

const Identifier *Control::objcRetainId() const
{ return d->objcRetainId; }

const Identifier *Control::objcCopyId() const
{ return d->objcCopyId; }

const Identifier *Control::objcNonatomicId() const
{ return d->objcNonatomicId; }
