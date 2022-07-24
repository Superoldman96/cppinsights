/******************************************************************************
 *
 * C++ Insights, copyright (C) by Andreas Fertig
 * Distributed under an MIT license. See LICENSE for details
 *
 ****************************************************************************/

#include "TemplateHandler.h"
#include <type_traits>
#include "ClangCompat.h"
#include "CodeGenerator.h"
#include "InsightsHelpers.h"
#include "InsightsMatchers.h"
#include "OutputFormatHelper.h"
//-----------------------------------------------------------------------------

using namespace clang;
using namespace clang::ast_matchers;
//-----------------------------------------------------------------------------

namespace clang::ast_matchers {
const internal::VariadicDynCastAllOfMatcher<Decl, VarTemplateDecl> varTemplateDecl;  // NOLINT

const internal::VariadicDynCastAllOfMatcher<Decl, ConceptDecl> conceptDecl;
}  // namespace clang::ast_matchers

namespace clang::insights {

/// \brief Find a \c DeclRefExpr belonging to a \c DecompisitionDecl
class CoroutineBodyFinder : public ConstStmtVisitor<CoroutineBodyFinder>
{
    bool mIsCoroutine{};

public:
    CoroutineBodyFinder() = default;

    void VisitCoroutineBodyStmt(const CoroutineBodyStmt*) { mIsCoroutine = true; }

    void VisitStmt(const Stmt* stmt)
    {
        if(isa<CoroutineBodyStmt>(stmt)) {
            mIsCoroutine = true;
            return;
        }

        for(const auto* child : stmt->children()) {
            if(child) {
                Visit(child);
            }

            if(mIsCoroutine) {
                return;
            }
        }
    }

    bool Find(const Stmt* stmt)
    {
        if(stmt) {
            VisitStmt(stmt);
        }

        return mIsCoroutine;
    }
};
//-----------------------------------------------------------------------------

/// \brief Insert the instantiated template with the resulting code.
static OutputFormatHelper InsertInstantiatedTemplate(const Decl* decl)
{
    OutputFormatHelper outputFormatHelper{};
    outputFormatHelper.AppendNewLine();
    outputFormatHelper.AppendNewLine();

    CodeGenerator codeGenerator{outputFormatHelper};
    codeGenerator.InsertArg(decl);

    return outputFormatHelper;
}
//-----------------------------------------------------------------------------

TemplateHandler::TemplateHandler(Rewriter& rewrite, MatchFinder& matcher)
: InsightsBase(rewrite)
{
    matcher.addMatcher(functionDecl(allOf(unless(isExpansionInSystemHeader()),
                                          unless(isMacroOrInvalidLocation()),
                                          hasParent(functionTemplateDecl(hasParent(translationUnitDecl()))),
                                          isTemplateInstantiationPlain()))
                           .bind("func"),
                       this);

    matcher.addMatcher(functionTemplateDecl(hasThisTUParent).bind("func2"), this);

    // match typical use where a class template is defined and it is used later.
    matcher.addMatcher(classTemplateSpecializationDecl(
                           unless(isExpansionInSystemHeader()),
                           allOf(hasParent(classTemplateDecl(hasParent(translationUnitDecl())).bind("decl")),
                                 isExplicitTemplateSpecialization()))
                           .bind("class"),
                       this);

    matcher.addMatcher(classTemplateDecl(hasThisTUParent).bind("decl"), this);

    matcher.addMatcher(conceptDecl(hasThisTUParent).bind("decl"), this);

    // special case, where a class template is defined and somewhere else we request an explicit instantiation
    matcher.addMatcher(
        classTemplateSpecializationDecl(hasThisTUParent, unless(isExplicitTemplateSpecialization())).bind("class"),
        this);

    matcher.addMatcher(varTemplateDecl(hasThisTUParent).bind("vd"), this);
}
//-----------------------------------------------------------------------------

// Special treatment for function templates which end with a semi-colon but have a valid body. In theory getting the end
// of the location should be equivalent to what is done in FunctionDeclHandler.cpp. However, this approach fails for
// example for AutoHandler2Test.cpp.
static SourceLocation FindLocationAfterRBrace(const SourceLocation                          loc,
                                              const ast_matchers::MatchFinder::MatchResult& result,
                                              const bool                                    mustHaveSemi)
{
    if(not mustHaveSemi) {
        auto nToken = clang::Lexer::findNextToken(loc, GetSM(result), result.Context->getLangOpts());

        if(nToken.hasValue()) {
            if(auto& rToken = nToken.getValue(); rToken.getKind() == tok::semi) {
                return rToken.getLocation();
            }
        }
    }

    return FindLocationAfterSemi(loc, result).getLocWithOffset(1);
}
//-----------------------------------------------------------------------------

void TemplateHandler::run(const MatchFinder::MatchResult& result)
{
    if(const auto* functionDecl = result.Nodes.getNodeAs<FunctionDecl>("func")) {
        if(not functionDecl->getBody() && not isa<CXXDeductionGuideDecl>(functionDecl)) {
            return;
        }

        // Figure out whether we are looking at a CXXDeductionGuideDecl. If so, ensure that the deduction guide comes
        // after the primary template declaration.
        const Decl* endLocDecl = functionDecl;
        if(const auto* deductionGuide = dyn_cast_or_null<CXXDeductionGuideDecl>(functionDecl)) {
            const auto* deducedTemplate = deductionGuide->getDeducedTemplate();

            // deduction guide must follow after the template declaration
            if(deducedTemplate->getEndLoc() > deductionGuide->getEndLoc()) {
                endLocDecl = deducedTemplate;
            }
        }

        OutputFormatHelper outputFormatHelper = InsertInstantiatedTemplate(functionDecl);
        const auto         endOfCond          = FindLocationAfterRBrace(
            endLocDecl->getEndLoc(),
            result,
            isa<CXXDeductionGuideDecl>(
                functionDecl));  // if we lift the "not getBody()" restriction above we need to take this in account
                                                  // here, as then we require a semi at the end.

        InsertIndentedText(endOfCond, outputFormatHelper);

    } else if(const auto* functionTemplateDecl = result.Nodes.getNodeAs<FunctionTemplateDecl>("func2")) {
        if(isa<CXXDeductionGuideDecl>(functionTemplateDecl->getTemplatedDecl())) {
            return;
        }

        // For now, don't transform the primary template of a coroutine
        if(CoroutineBodyFinder{}.Find(functionTemplateDecl->getTemplatedDecl()->getBody())) {
            return;
        }

        OutputFormatHelper outputFormatHelper{};

        CodeGenerator codeGenerator{outputFormatHelper};
        codeGenerator.InsertPrimaryTemplate(functionTemplateDecl);

        mRewrite.ReplaceText(functionTemplateDecl->getSourceRange(), outputFormatHelper.GetString());

    } else if(const auto* clsTmplSpecDecl = result.Nodes.getNodeAs<ClassTemplateSpecializationDecl>("class")) {
        // skip classes/struct's without a definition
        if(not clsTmplSpecDecl->hasDefinition()) {
            return;
        }

        OutputFormatHelper outputFormatHelper = InsertInstantiatedTemplate(clsTmplSpecDecl);

        if(const auto* clsTmplDecl = result.Nodes.getNodeAs<ClassTemplateDecl>("decl")) {
            const auto endOfCond = FindLocationAfterSemi(clsTmplDecl->getEndLoc(), result);
            InsertIndentedText(endOfCond, outputFormatHelper);

        } else {  // explicit specialization, we have to remove the specialization
            mRewrite.ReplaceText(clsTmplSpecDecl->getSourceRange(), outputFormatHelper.GetString());
        }

    } else if(const auto* clsTmplDecl = result.Nodes.getNodeAs<ClassTemplateDecl>("decl")) {

        OutputFormatHelper outputFormatHelper{};

        CodeGenerator codeGenerator{outputFormatHelper};
        codeGenerator.InsertArg(clsTmplDecl);

        mRewrite.ReplaceText(
            {clsTmplDecl->getSourceRange().getBegin(), FindLocationAfterSemi(clsTmplDecl->getEndLoc(), result)},
            outputFormatHelper.GetString());

    } else if(const auto* conceptDecl = result.Nodes.getNodeAs<ConceptDecl>("decl")) {

        OutputFormatHelper outputFormatHelper{};

        CodeGenerator codeGenerator{outputFormatHelper};
        codeGenerator.InsertArg(conceptDecl);

        mRewrite.ReplaceText({conceptDecl->getSourceRange().getBegin(),
                              FindLocationAfterSemi(conceptDecl->getEndLoc(), result).getLocWithOffset(1)},
                             outputFormatHelper.GetString());

    } else if(const auto* vd = result.Nodes.getNodeAs<VarTemplateDecl>("vd")) {
        OutputFormatHelper outputFormatHelper = InsertInstantiatedTemplate(vd);

        if(auto sourceRange = vd->getSourceRange(); not IsMacroLocation(sourceRange)) {
            const auto endOfCond = FindLocationAfterSemi(vd->getEndLoc(), result);

            mRewrite.ReplaceText({sourceRange.getBegin(), endOfCond.getLocWithOffset(1)},
                                 outputFormatHelper.GetString());

        } else {
            // We're just interested in the start location, -1 work(s|ed)
            const auto startLoc =
                GetSourceRangeAfterSemi(sourceRange, result, RequireSemi::No).getBegin().getLocWithOffset(-1);

            InsertIndentedText(startLoc, outputFormatHelper);
        }
    }
}
//-----------------------------------------------------------------------------

}  // namespace clang::insights
