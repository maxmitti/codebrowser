/****************************************************************************
 * Copyright (C) 2012-2016 Woboq GmbH
 * Olivier Goffart <contact at woboq.com>
 * https://woboq.com/codebrowser.html
 *
 * This file is part of the Woboq Code Browser.
 *
 * Commercial License Usage:
 * Licensees holding valid commercial licenses provided by Woboq may use
 * this file in accordance with the terms contained in a written agreement
 * between the licensee and Woboq.
 * For further information see https://woboq.com/codebrowser.html
 *
 * Alternatively, this work may be used under a Creative Commons
 * Attribution-NonCommercial-ShareAlike 3.0 (CC-BY-NC-SA 3.0) License.
 * http://creativecommons.org/licenses/by-nc-sa/3.0/deed.en_US
 * This license does not allow you to use the code browser to assist the
 * development of your commercial software. If you intent to do so, consider
 * purchasing a commercial licence.
 ****************************************************************************/

/* This file handle the support of the SIGNAL and SLOT macro in QObject::connect */

#include "qtsupport.h"

#include <clang/AST/DeclCXX.h>
#include <clang/AST/ExprCXX.h>
#include <clang/AST/PrettyPrinter.h>
#include <clang/Basic/SourceManager.h>
#include <clang/Lex/Lexer.h>

#include <algorithm>
#include <string>

#include "annotator.h"

/**
 * Lookup candidates function of name \a methodName within the QObject derivative \a objClass
 * its bases, or its private implementation
 */
static llvm::SmallVector<clang::CXXMethodDecl *, 10>
lookUpCandidates(const clang::CXXRecordDecl *objClass, llvm::StringRef methodName)
{
    llvm::SmallVector<clang::CXXMethodDecl *, 10> candidates;
    clang::CXXMethodDecl *d_func = nullptr;
    auto classIt = objClass;
    while (classIt) {
        if (!classIt->getDefinition())
            break;

        for (auto mi = classIt->method_begin(); mi != classIt->method_end(); ++mi) {
            if (!(*mi)->getIdentifier())
                continue;
            if ((*mi)->getName() == methodName)
                candidates.push_back(*mi);
            if (!d_func && (*mi)->getName() == "d_func" && !getResultType(*mi).isNull())
                d_func = *mi;
        }

        // Look in the first base  (because the QObject need to be the first base class)
        classIt = classIt->getNumBases() == 0
            ? nullptr
            : classIt->bases_begin()->getType()->getAsCXXRecordDecl();

        if (d_func && !classIt && candidates.empty()) {
            classIt = getResultType(d_func)->getPointeeCXXRecordDecl();
            d_func = nullptr;
        }
    }
    return candidates;
}

/**
 * \a obj is an expression to a type of an QObject (or pointer to) that is the sender or the
 * receiver \a method is an expression like SIGNAL(....)  or SLOT(....)
 *
 * This function try to find the matching signal or slot declaration, and register its use.
 */
void QtSupport::handleSignalOrSlot(clang::Expr *obj, clang::Expr *method)
{
    if (!obj || !method)
        return;
    obj = obj->IgnoreImpCasts();
    method = method->IgnoreImpCasts();
    auto objType = obj->getType().getTypePtrOrNull();
    if (!objType)
        return;

    const clang::CXXRecordDecl *objClass = objType->getPointeeCXXRecordDecl();
    if (!objClass) {
        // It can be a non-pointer if called like:  foo.connect(....);
        objClass = objType->getAsCXXRecordDecl();
        if (!objClass)
            return;
    }

    const clang::StringLiteral *methodLiteral = clang::dyn_cast<clang::StringLiteral>(method);
    if (!methodLiteral) {
        // try qFlagLocation
        clang::CallExpr *flagLoc = clang::dyn_cast<clang::CallExpr>(method);

        if (!flagLoc || flagLoc->getNumArgs() != 1 || !flagLoc->getDirectCallee()
            || flagLoc->getDirectCallee()->getName() != "qFlagLocation")
            return;


        methodLiteral = clang::dyn_cast<clang::StringLiteral>(flagLoc->getArg(0)->IgnoreImpCasts());
        if (!methodLiteral)
            return;
    }
    if (methodLiteral->getCharByteWidth() != 1)
        return;


    auto signature = methodLiteral->getString().trim();
    if (signature.size() < 4)
        return;

    if (signature.find('\0') != signature.npos) {
        signature = signature.substr(0, signature.find('\0')).trim();
    }

    auto lParenPos = signature.find('(');
    auto rParenPos = signature.find(')');
    if (rParenPos == std::string::npos || rParenPos < lParenPos || lParenPos < 2)
        return;

    llvm::StringRef methodName = signature.slice(1, lParenPos).trim();

    // Try to find the method which match this name in the given class or bases.
    auto candidates = lookUpCandidates(objClass, methodName);

    clang::LangOptions lo;
    lo.CPlusPlus = true;
    lo.Bool = true;
    clang::PrintingPolicy policy(lo);
    policy.SuppressScope = true;

    auto argPos = lParenPos + 1;
    unsigned int arg = 0;
    while (argPos < signature.size() && !candidates.empty()) {

        // Find next comma to extract the next argument
        auto searchPos = argPos;
        while (searchPos < signature.size() && signature[searchPos] != ','
               && signature[searchPos] != ')') {
            if (signature[searchPos] == '<') {
                int depth = 0;
                int templDepth = 0;
                searchPos++;
                while (searchPos < signature.size() && depth >= 0 && templDepth >= 0) {
                    switch (signature[searchPos]) {
                    case '(':
                    case '[':
                    case '{':
                        depth++;
                        break;
                    case ')':
                    case ']':
                    case '}':
                        depth--;
                        break;
                    case '>':
                        if (depth == 0)
                            templDepth--;
                        break;
                    case '<':
                        if (depth == 0)
                            templDepth++;
                        break;
                    }
                    ++searchPos;
                }
                continue;
            }
            ++searchPos;
        }

        if (searchPos == signature.size())
            return;

        llvm::StringRef argument = signature.substr(argPos, searchPos - argPos).trim();
        // Skip the const at the beginning

        if (argument.starts_with("const ") && argument.ends_with("&"))
            argument = argument.slice(6, argument.size() - 1).trim();

        argPos = searchPos + 1;

        if (argument.empty() && signature[searchPos] == ')' && arg == 0)
            break; // No arguments


        // Now go over the candidates and prune the impossible ones.
        auto it = candidates.begin();
        while (it != candidates.end()) {
            if ((*it)->getNumParams() < arg + 1) {
                // Not enough argument
                it = candidates.erase(it);
                continue;
            }

            auto type = (*it)->getParamDecl(arg)->getType();

            // remove const or const &
            if (type->isReferenceType() && type.getNonReferenceType().isConstQualified())
                type = type.getNonReferenceType();
            type.removeLocalConst();

            auto typeString_ = type.getAsString(policy);

            auto typeString = llvm::StringRef(typeString_).trim();

            // Now compare the two string without mathcin spaces,
            auto sigIt = argument.begin();
            auto parIt = typeString.begin();
            while (sigIt != argument.end() && parIt != typeString.end()) {
                if (*sigIt == *parIt) {
                    ++sigIt;
                    ++parIt;
                } else if (*sigIt == ' ') {
                    ++sigIt;
                } else if (*parIt == ' ') {
                    ++parIt;
                } else if (*sigIt == 'n' && llvm::StringRef(sigIt, 9).starts_with("nsigned ")) {
                    // skip unsigned
                    sigIt += 8;
                } else if (*parIt == 'n' && llvm::StringRef(parIt, 9).starts_with("nsigned ")) {
                    // skip unsigned
                    parIt += 8;
                } else {
                    break;
                }
            }

            if (sigIt != argument.end() || parIt != typeString.end()) {
                // Did not match.
                it = candidates.erase(it);
                continue;
            }

            ++it;
        }

        arg++;
        if (signature[searchPos] == ')')
            break;
    }

    if (argPos != signature.size())
        return;


    // Remove candidates that needs more argument
    candidates.erase(
        std::remove_if(candidates.begin(), candidates.end(),
                       [=](clang::CXXMethodDecl *it) {
                           return it->getMinRequiredArguments() > arg
                               && !(it->getNumParams() == arg + 1
                                    && it->getParamDecl(arg)->getType().getAsString(policy)
                                        == "QPrivateSignal");
                       }),
        candidates.end());

    if (candidates.empty())
        return;

    auto used = candidates.front();



    clang::SourceRange range = methodLiteral->getSourceRange();
    if (methodLiteral->getNumConcatenated() >= 2) {
        auto &sm = annotator.getSourceMgr();
        // Goes two level up in the macro expension:  First level is the # expansion,  Second level
        // is SIGNAL macro
        auto r = sm.getImmediateExpansionRange(methodLiteral->getStrTokenLoc(1));
        range = { sm.getImmediateExpansionRange(r.getBegin()).getBegin(),
                  sm.getImmediateExpansionRange(r.getEnd()).getEnd() };

        // now remove the SIGNAL or SLOT macro from the range.
        auto skip = clang::Lexer::MeasureTokenLength(range.getBegin(), sm, annotator.getLangOpts());
        range.setBegin(range.getBegin().getLocWithOffset(skip + 1));
        // remove the ')' while we are on it
        range.setEnd(range.getEnd().getLocWithOffset(-1));
    }

    annotator.registerUse(used, range, Annotator::Call, currentContext, Annotator::Use_Address);
}

/**
 * Very similar to handleSignalOrSlot, but does not handle the fact that the string might be in a
 * macro and does the string contains the method name and not the full signature \a obj is an
 * expression to a type of an QObject (or pointer to) that is the sender or the receiver \a method
 * is an expression of type char*
 *
 * TODO: handle overloads
 */
void QtSupport::handleInvokeMethod(clang::Expr *obj, clang::Expr *method)
{
    if (!obj || !method)
        return;
    obj = obj->IgnoreImpCasts();
    method = method->IgnoreImpCasts();
    auto objType = obj->getType().getTypePtrOrNull();
    if (!objType)
        return;
    const clang::CXXRecordDecl *objClass = objType->getPointeeCXXRecordDecl();
    if (!objClass)
        return;

    const clang::StringLiteral *methodLiteral = clang::dyn_cast<clang::StringLiteral>(method);
    if (!methodLiteral)
        return;
    if (methodLiteral->getCharByteWidth() != 1)
        return;

    auto methodName = methodLiteral->getString();
    if (methodName.empty())
        return;

    // Try to find the method which match this name in the given class or bases.
    auto candidates = lookUpCandidates(objClass, methodName);
    if (candidates.size() != 1)
        return;
    // FIXME: overloads resolution using the Q_ARG

    auto used = candidates.front();
    clang::SourceRange range = methodLiteral->getSourceRange();
    annotator.registerUse(used, range, Annotator::Call, currentContext, Annotator::Use_Address);
}

void QtSupport::visitCallExpr(clang::CallExpr *e)
{
    clang::CXXMethodDecl *methodDecl =
        clang::dyn_cast_or_null<clang::CXXMethodDecl>(e->getCalleeDecl());
    if (!methodDecl || !methodDecl->getDeclName().isIdentifier() || !methodDecl->getParent())
        return;
    if (!methodDecl->getParent()->getDeclName().isIdentifier())
        return;

    auto parentName = methodDecl->getParent()->getName();

    if (!parentName.starts_with("Q"))
        return; // only Qt classes

    if (parentName == "QObject"
        && (methodDecl->getName() == "connect" || methodDecl->getName() == "disconnect")) {
        // We have a call to QObject::connect or disconnect
        if (methodDecl->isStatic()) {
            if (e->getNumArgs() >= 4) {
                handleSignalOrSlot(e->getArg(0), e->getArg(1));
                handleSignalOrSlot(e->getArg(2), e->getArg(3));
            }
        } else if (clang::CXXMemberCallExpr *me = clang::dyn_cast<clang::CXXMemberCallExpr>(e)) {
            if (e->getNumArgs() >= 3) {
                handleSignalOrSlot(e->getArg(0), e->getArg(1));
                handleSignalOrSlot(me->getImplicitObjectArgument(), e->getArg(2));
            }
        }
    }
    if (parentName == "QTimer" && methodDecl->getName() == "singleShot") {
        if (e->getNumArgs() >= 3) {
            handleSignalOrSlot(e->getArg(1), e->getArg(2));
        }
    }
    if (parentName == "QHostInfo" && methodDecl->getName() == "lookupHost") {
        if (e->getNumArgs() >= 3) {
            handleSignalOrSlot(e->getArg(1), e->getArg(2));
        }
    }
    if (parentName == "QNetworkAccessCache" && methodDecl->getName() == "requestEntry") {
        if (e->getNumArgs() >= 3) {
            handleSignalOrSlot(e->getArg(1), e->getArg(2));
        }
    }
    if (parentName == "QDBusAbstractInterface" && methodDecl->getName() == "callWithCallback") {
        if (e->getNumArgs() == 4) {
            handleSignalOrSlot(e->getArg(2), e->getArg(3));
        } else if (e->getNumArgs() == 5) {
            handleSignalOrSlot(e->getArg(2), e->getArg(3));
            handleSignalOrSlot(e->getArg(2), e->getArg(4));
        }
    }
    if (methodDecl->getName() == "open"
        && (parentName == "QFileDialog" || parentName == "QColorDialog"
            || parentName == "QFontDialog" || parentName == "QMessageBox"
            || parentName == "QInputDialog" || parentName == "QPrintDialog"
            || parentName == "QPageSetupDialog" || parentName == "QPrintPreviewDialog"
            || parentName == "QProgressDialog")) {
        if (e->getNumArgs() == 2) {
            handleSignalOrSlot(e->getArg(0), e->getArg(1));
        }
    }
    if (parentName == "QMenu" && methodDecl->getName() == "addAction") {
        if (methodDecl->getNumParams() == 4 && e->getNumArgs() >= 3) {
            handleSignalOrSlot(e->getArg(1), e->getArg(2));
        } else if (methodDecl->getNumParams() == 5 && e->getNumArgs() >= 4) {
            handleSignalOrSlot(e->getArg(2), e->getArg(3));
        }
    }
    if (parentName == "QToolbar" && methodDecl->getName() == "addAction") {
        if (e->getNumArgs() == 3) {
            handleSignalOrSlot(e->getArg(1), e->getArg(2));
        } else if (e->getNumArgs() == 4) {
            handleSignalOrSlot(e->getArg(2), e->getArg(3));
        }
    }
    if (parentName == "QState" && methodDecl->getName() == "addTransition") {
        if (e->getNumArgs() >= 2) {
            handleSignalOrSlot(e->getArg(0), e->getArg(1));
        }
    }
    if (parentName == "QMetaObject" && methodDecl->getName() == "invokeMethod") {
        if (e->getNumArgs() >= 2) {
            handleInvokeMethod(e->getArg(0), e->getArg(1));
        }
    }
}

void QtSupport::visitCXXConstructExpr(clang::CXXConstructExpr *e)
{
    clang::CXXConstructorDecl *methodDecl = e->getConstructor();
    if (!methodDecl || !methodDecl->getParent())
        return;

    auto parent = methodDecl->getParent();
    if (!parent->getName().starts_with("Q"))
        return; // only Qt classes

    if (parent->getName() == "QShortcut") {
        if (e->getNumArgs() >= 3)
            handleSignalOrSlot(e->getArg(1), e->getArg(2));
        if (e->getNumArgs() >= 4)
            handleSignalOrSlot(e->getArg(1), e->getArg(3));
    }
    if (parent->getName() == "QSignalSpy" || parent->getName() == "QSignalTransition") {
        if (e->getNumArgs() >= 2) {
            handleSignalOrSlot(e->getArg(0), e->getArg(1));
        }
    }
}
