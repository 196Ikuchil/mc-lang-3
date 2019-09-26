//===----------------------------------------------------------------------===//
// Code Generation
// このファイルでは、parser.hによって出来たASTからLLVM IRを生成しています。
// といっても、難しいことをしているわけではなく、IRBuilder(https://llvm.org/doxygen/IRBuilder_8h_source.html)
// のインターフェースを利用して、parser.hで定義した「ソースコードの意味」をIRに落としています。
// 各ファイルの中で一番LLVMの機能を多用しているファイルです。
//===----------------------------------------------------------------------===//

// https://llvm.org/doxygen/LLVMContext_8h_source.html
static LLVMContext Context;
// https://llvm.org/doxygen/classllvm_1_1IRBuilder.html
// LLVM IRを生成するためのインターフェース
static IRBuilder<> Builder(Context);
// https://llvm.org/doxygen/classllvm_1_1Module.html
// このModuleはC++ Moduleとは何の関係もなく、LLVM IRを格納するトップレベルオブジェクトです。
static std::unique_ptr<Module> myModule;
// 変数名とllvm::Valueのマップを保持する
// 変数の名前(str)に対応する値(AllocaInst?)を対応づけているはず
static std::map<std::string, AllocaInst *> NamedValues;
//この中にグローバル変数を追加していきたい
static std::map<std::string, AllocaInst *> GlobalVariables;
// 最適化に利用する
static std::unique_ptr<legacy::FunctionPassManager> TheFPM;

// https://llvm.org/doxygen/classllvm_1_1Value.html
// llvm::Valueという、LLVM IRのオブジェクトでありFunctionやModuleなどを構成するクラスを使います
Value *NumberAST::codegen() {
    // 64bit整数型のValueを返す
    return ConstantInt::get(Context, APInt(64, Val, true));
}

Value *LogErrorV(const char *str) {
    LogError(str);
    return nullptr;
}

/// CreateEntryBlockAlloca - alloca命令を関数のentryブロックに生成する。
/// これは変更可能な変数などのために使われる。
static AllocaInst *CreateEntryBlockAlloca(Function *function, const std::string &VarName){
    IRBuilder<> TmpB(&function->getEntryBlock(),function->getEntryBlock().begin());
    return TmpB.CreateAlloca(Type::getInt64Ty(Context),0,VarName.c_str());
}

// TODO 2.4: 引数のcodegenを実装してみよう
Value *VariableExprAST::codegen() {
    // NamedValuesの中にVariableExprAST::NameとマッチするValueがあるかチェックし、
    // あったらそのValueを返す。
    Value *V = NamedValues[variableName];
    // if (!V)
    //     *V = GlobalVariables[variableName]; //グローバル変数の中から探してみる
    if (!V)
        return LogErrorV("Unknown variable name");
    return Builder.CreateLoad(V,variableName.c_str());
}

Value *GlobalVariableExprAST::codegen(){
    LogError("this is global declaration.");
    return nullptr;
}

// TODO 2.5: 関数呼び出しのcodegenを実装してみよう
Value *CallExprAST::codegen() {
    // 1. myModule->getFunctionを用いてcalleeがdefineされているかを
    // チェックし、されていればそのポインタを得る。
    Function *CalleeF = myModule->getFunction(callee);
    if (!CalleeF)
        return LogErrorV("Unknown function referenced");

    // 2. llvm::Function::arg_sizeと実際に渡されたargsのサイズを比べ、
    // サイズが間違っていたらエラーを出力。
    if (CalleeF->arg_size() != args.size())
        return LogErrorV("Incorrect # arguments passed");

    std::vector<Value *> argsV;
    // 3. argsをそれぞれcodegenしllvm::Valueにし、argsVにpush_backする。
    for (unsigned i = 0, e = args.size(); i != e; ++i) {
        argsV.push_back(args[i]->codegen());
        if (!argsV.back())
            return nullptr;
    }

    // 4. IRBuilderのCreateCallを呼び出し、Valueをreturnする。
    return Builder.CreateCall(CalleeF, argsV, "calltmp");
}

Value *BinaryAST::codegen() {
    // 二項演算子の両方の引数をllvm::Valueにする。
    Value *L = LHS->codegen();
    Value *R = RHS->codegen();
    if (!L || !R)
        return nullptr;

    switch (Op) {
        case '+':
            // LLVM IR Builerを使い、この二項演算のIRを作る
            return Builder.CreateAdd(L, R, "addtmp");
            // TODO 1.7: '-'と'*'に対してIRを作ってみよう
            // 上の行とhttps://llvm.org/doxygen/classllvm_1_1IRBuilder.htmlを参考のこと
        case '-':
            return Builder.CreateSub(L, R, "subtmp");
        case '*':
            return Builder.CreateMul(L, R, "multmp");
        // TODO 3.1: '<'を実装してみよう
        // '<'のcodegenを実装して下さい。その際、以下のIRBuilderのメソッドが使えます。
        // CreateICmp: https://llvm.org/doxygen/classllvm_1_1IRBuilder.html#a103d309fa238e186311cbeb961b5bcf4
        // llvm::CmpInst::ICMP_SLT: https://llvm.org/doxygen/classllvm_1_1CmpInst.html#a283f9a5d4d843d20c40bb4d3e364bb05
        // CreateIntCast: https://llvm.org/doxygen/classllvm_1_1IRBuilder.html#a5bb25de40672dedc0d65e608e4b78e2f
        // CreateICmpの返り値がi1(1bit)なので、CreateIntCastはそれをint64にcastするのに用います。
        case '<':
            return Builder.CreateIntCast(Builder.CreateICmp(llvm::CmpInst::ICMP_SLT ,L,R,"slttmp"),Type::getInt64Ty(Context),true ,"cast_i1_to_i64");
        default:
            return LogErrorV("invalid binary operator");
    }
}

Function *PrototypeAST::codegen() {
    // MC言語では変数の型も関数の返り値もintの為、関数の返り値をInt64にする。
    std::vector<Type *> prototype(args.size(), Type::getInt64Ty(Context));
    FunctionType *FT =
        FunctionType::get(Type::getInt64Ty(Context), prototype, false);
    // https://llvm.org/doxygen/classllvm_1_1Function.html
    // llvm::Functionは関数のIRを表現するクラス
    Function *F =
        Function::Create(FT, Function::ExternalLinkage, Name, myModule.get());

    // 引数の名前を付ける
    unsigned i = 0;
    for (auto &Arg : F->args())
        Arg.setName(args[i++]);

    return F;
}


Function *FunctionAST::codegen() {
    // この関数が既にModuleに登録されているか確認
    Function *function = myModule->getFunction(proto->getFunctionName());
    // 関数名が見つからなかったら、新しくこの関数のIRクラスを作る。
    if (!function)
        function = proto->codegen();
    if (!function)
        return nullptr;

    // エントリーポイントを作る
    BasicBlock *BB = BasicBlock::Create(Context, "entry", function);
    Builder.SetInsertPoint(BB);

    // Record the function arguments in the NamedValues map.
    NamedValues.clear();
    for (auto &Arg : function->args()){
        // この変数のためのallocaを生成する。
        AllocaInst *Alloca = CreateEntryBlockAlloca(function,Arg.getName());
        // allocaの中に初期値を保存する。
        Builder.CreateStore(&Arg,Alloca);
        // 引数をシンボルテーブルに追加する。
        NamedValues[Arg.getName()] = Alloca;
    }
    // 関数のbody(ExprASTから継承されたNumberASTかBinaryAST)をcodegenする
    if (Value *RetVal = body->codegen()) {
        // returnのIRを作る
        Builder.CreateRet(RetVal);

        // https://llvm.org/doxygen/Verifier_8h.html
        // 関数の検証
        verifyFunction(*function);
        //最適化かな？
        TheFPM->run(*function);

        return function;
    }

    // もし関数のbodyがnullptrなら、この関数をModuleから消す。
    function->eraseFromParent();
    return nullptr;
}

Value *IfExprAST::codegen() {
    // if x < 5 then x + 3 else x - 5;
    // というコードが入力だと考える。
    // Cond->codegen()によって"x < 5"のcondition部分がcodegenされ、その返り値(int)が
    // CondVに格納される。
    Value *CondV = Cond->codegen();
    if (!CondV)
        return nullptr;

    // CondVはint64でtrueなら0以外、falseなら0が入っているため、CreateICmpNEを用いて
    // CondVが0(false)とnot-equalかどうか判定し、CondVをbool型にする。
    CondV = Builder.CreateICmpNE(
            CondV, ConstantInt::get(Context, APInt(64, 0)), "ifcond");
    // if文を呼んでいる関数の名前
    Function *ParentFunc = Builder.GetInsertBlock()->getParent();

    // "thenだった場合"と"elseだった場合"のブロックを作り、ラベルを付ける。
    // "ifcont"はif文が"then"と"else"の処理の後、二つのコントロールフローを
    // マージするブロック。
    BasicBlock *ThenBB =
        BasicBlock::Create(Context, "then", ParentFunc);
    BasicBlock *ElseBB = BasicBlock::Create(Context, "else");
    BasicBlock *MergeBB = BasicBlock::Create(Context, "ifcont");
    // condition, trueだった場合のブロック、falseだった場合のブロックを登録する。
    // https://llvm.org/doxygen/classllvm_1_1IRBuilder.html#a3393497feaca1880ab3168ee3db1d7a4
    Builder.CreateCondBr(CondV, ThenBB, ElseBB);

    // "then"のブロックを作り、その内容(expression)をcodegenする。
    Builder.SetInsertPoint(ThenBB);
    Value *ThenV = Then->codegen();
    if (!ThenV)
        return nullptr;
    // "then"のブロックから出る時は"ifcont"ブロックに飛ぶ。
    Builder.CreateBr(MergeBB);
    // ThenBBをアップデートする。
    ThenBB = Builder.GetInsertBlock();

    // TODO 3.4: "else"ブロックのcodegenを実装しよう
    // "then"ブロックを参考に、"else"ブロックのcodegenを実装して下さい。
    Builder.SetInsertPoint(ElseBB);
    Value *ElseV = Else->codegen();
    if (!ElseV) return nullptr;
    Builder.CreateBr(MergeBB);
    ElseBB = Builder.GetInsertBlock();
    // 注意: 20行下のコメントアウトを外して下さい。
    ParentFunc->getBasicBlockList().push_back(ElseBB);

    // "ifcont"ブロックのcodegen
    ParentFunc->getBasicBlockList().push_back(MergeBB);
    Builder.SetInsertPoint(MergeBB);
    // https://llvm.org/docs/LangRef.html#phi-instruction
    // PHINodeは、"then"ブロックのValueか"else"ブロックのValue
    // どちらをifブロック全体の返り値にするかを実行時に選択します。
    // もちろん、"then"ブロックに入るconditionなら前者が選ばれ、そうでなければ後者な訳です。
    // LLVM IRはSSAという"全ての変数が一度だけassignされる"規約があるため、
    // 値を上書きすることが出来ません。従って、このように実行時にコントロールフローの
    // 値を選択する機能が必要です。
    PHINode *PN =
        Builder.CreatePHI(Type::getInt64Ty(Context), 2, "iftmp");

    PN->addIncoming(ThenV, ThenBB);
    // TODO 3.4:を実装したらコメントアウトを外して下さい。
    PN->addIncoming(ElseV, ElseBB);
    return PN;
}

//===----------------------------------------------------------------------===//
// MC コンパイラエントリーポイント
// mc.cppでMainLoop()が呼ばれます。MainLoopは各top level expressionに対して
// HandleTopLevelExpressionを呼び、その中でASTを作り再帰的にcodegenをしています。
//===----------------------------------------------------------------------===//
static void InitializeModuleAndPassManager() {
    myModule = llvm::make_unique<Module>("my cool jit", Context);

    // Create a new pass manager attached to it.
    TheFPM = make_unique<legacy::FunctionPassManager>(myModule.get());

    // Promote allocas to registers. 追加したmem2reg
    TheFPM->add(createPromoteMemoryToRegisterPass());
    // Do simple "peephole" optimizations and bit-twiddling optzns.
    TheFPM->add(createInstructionCombiningPass());
    // Reassociate expressions.
    TheFPM->add(createReassociatePass());
    // Eliminate Common SubExpressions.
    TheFPM->add(createGVNPass());
    // Simplify the control flow graph (deleting unreachable blocks, etc).
    TheFPM->add(createCFGSimplificationPass());

    TheFPM->doInitialization();
}

static std::string streamstr;
static llvm::raw_string_ostream stream(streamstr);
static void HandleDefinition() {
    if (auto FnAST = ParseDefinition()) {
        if (auto *FnIR = FnAST->codegen()) {
            FnIR->print(stream);
            InitializeModuleAndPassManager();
        }
    } else {
        getNextToken();
    }
}

static void HandleIntDefinition(){
    if (auto IntAST = ParseIntDefinition()){ //構文をパース
        if (auto *IntIR = IntAST->codegen()){
            IntIR->print(stream);
            // InitializeModuleAndPassManager();
        }
    } else {
        getNextToken();
    }
}

// その名の通りtop level expressionをcodegenします。例えば、「2+1;3+3;」というファイルが
// 入力だった場合、この関数は最初の「2+1」をcodegenして返ります。(そしてMainLoopからまだ呼び出されます)
static void HandleTopLevelExpression() {
    // ここでテキストファイルを全てASTにします。
    if (auto FnAST = ParseTopLevelExpr()) {
        // できたASTをcodegenします。
        if (auto *FnIR = FnAST->codegen()) {
            InitializeModuleAndPassManager();
            streamstr = "";
            FnIR->print(stream);
        }
    } else {
        // エラー
        getNextToken();
    }
}

static void MainLoop() {
    InitializeModuleAndPassManager();
    while (true) {
        switch (CurTok) {
            case tok_eof:
                // ここで最終的なLLVM IRをプリントしています。
                fprintf(stderr, "%s", stream.str().c_str());
                return;
            case tok_def:
                HandleDefinition();
                break;
            case tok_int:
                HandleIntDefinition();
                break;
            case ';': // ';'で始まった場合、無視します
                getNextToken();
                break;
            default:
                HandleTopLevelExpression();
                break;
        }
    }
}
