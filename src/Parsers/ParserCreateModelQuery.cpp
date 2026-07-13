#include <Parsers/ParserCreateModelQuery.h>
#include <Parsers/ASTCreateModelQuery.h>
#include <Parsers/ASTIdentifier.h>
#include <Parsers/CommonParsers.h>
#include <Parsers/ParserSetQuery.h>
#include <Parsers/ExpressionElementParsers.h>
#include <Parsers/ExpressionListParsers.h>

namespace DB
{

bool ParserCreateModelQuery::parseImpl(IParser::Pos & pos, ASTPtr & node, Expected & expected)
{
    auto model = make_intrusive<ASTCreateModelQuery>();

    ParserKeyword keyword_create(Keyword::CREATE);
    ParserKeyword keyword_model(Keyword::MODEL);
    ParserKeyword keyword_algorithm(Keyword::ALGORITHM);
    ParserKeyword keyword_options(Keyword::OPTIONS);
    ParserKeyword keyword_target(Keyword::TARGET);
    ParserKeyword keyword_from(Keyword::FROM);
    ParserKeyword keyword_table(Keyword::TABLE);

    ParserCompoundIdentifier model_name_parameter;
    ParserStringLiteral algorithm_parameter;
    ParserSetQuery options_parameter(true);
    ParserStringLiteral target_parameter;
    ParserCompoundIdentifier table_name_parameter;

    // CREATE MODEL

    if (!keyword_create.ignore(pos, expected))
        return false;

    if (!keyword_model.ignore(pos, expected))
        return false;

    ASTPtr model_name;
    if (!model_name_parameter.parse(pos, model_name, expected))
        return false;

    // ALGORITHM

    if (!keyword_algorithm.ignore(pos, expected))
        return false;

    ASTPtr algorithm;
    if (!algorithm_parameter.parse(pos, algorithm, expected))
        return false;

    // OPTIONS (optional field)

    ASTPtr options;
    if (keyword_options.ignore(pos, expected))
    {
        if (!ParserToken(TokenType::OpeningRoundBracket).ignore(pos, expected))
            return false;

        if (!options_parameter.parse(pos, options, expected))
            return false;

        if (!ParserToken(TokenType::ClosingRoundBracket).ignore(pos, expected))
            return false;
    }

    // TARGET

    if (!keyword_target.ignore(pos, expected))
        return false;

    ASTPtr target;
    if (!target_parameter.parse(pos, target, expected))
        return false;

    // FROM TABLE

    if (!keyword_from.ignore(pos, expected))
        return false;

    if (!keyword_table.ignore(pos, expected))
        return false;

    ASTPtr table_name;
    if (!table_name_parameter.parse(pos, table_name, expected))
        return false;

    model->model_name = model_name;
    model->algorithm = algorithm;
    model->options = options;
    model->target = target;
    model->table_name = table_name;

    node = model;
    return true;
}

}
