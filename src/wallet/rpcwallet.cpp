// Copyright (c) 2010 Satoshi Nakamoto
// Copyright (c) 2009-2017 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <amount.h>
#include <base58.h>
#include <chain.h>
#include <consensus/validation.h>
#include <core_io.h>
#include <httpserver.h>
#include <validation.h>
#include <net.h>
#include <policy/feerate.h>
#include <policy/fees.h>
#include <policy/policy.h>
#include <policy/rbf.h>
#include <rpc/mining.h>
#include <rpc/safemode.h>
#include <rpc/server.h>
#include <rpc/util.h>
#include <script/sign.h>
#include <timedata.h>
#include <util.h>
#include <utilmoneystr.h>
#include <wallet/coincontrol.h>
#include <wallet/feebumper.h>
#include <wallet/wallet.h>
#include <wallet/walletdb.h>
#include <wallet/walletutil.h>
#include <pos/miner.h>
#include <rpc/blockchain.h>
#include <warnings.h>
#include <validation.h>
#include "zerocoin/zerocoin.h"
#include "../libzerocoin/Zerocoin.h"
#include "ghost-address/commitmentkey.h"
#include <vector>

#include <init.h>  // For StartShutdown

#include <stdint.h>

#include <univalue.h>

static const std::string WALLET_ENDPOINT_BASE = "/wallet/";

CWallet *GetWalletForJSONRPCRequest(const JSONRPCRequest& request)
{
    if (request.URI.substr(0, WALLET_ENDPOINT_BASE.size()) == WALLET_ENDPOINT_BASE) {
        // wallet endpoint was used
        std::string requestedWallet = urlDecode(request.URI.substr(WALLET_ENDPOINT_BASE.size()));
        for (CWalletRef pwallet : ::vpwallets) {
            if (pwallet->GetName() == requestedWallet) {
                return pwallet;
            }
        }
        throw JSONRPCError(RPC_WALLET_NOT_FOUND, "Requested wallet does not exist or is not loaded");
    }
    return ::vpwallets.size() == 1 || (request.fHelp && ::vpwallets.size() > 0) ? ::vpwallets[0] : nullptr;
}

std::string HelpRequiringPassphrase(CWallet * const pwallet)
{
    return pwallet && pwallet->IsCrypted()
        ? "\nRequires wallet passphrase to be set with walletpassphrase call."
        : "";
}

bool EnsureWalletIsAvailable(CWallet * const pwallet, bool avoidException)
{
    if (pwallet) return true;
    if (avoidException) return false;
    if (::vpwallets.empty()) {
        // Note: It isn't currently possible to trigger this error because
        // wallet RPC methods aren't registered unless a wallet is loaded. But
        // this error is being kept as a precaution, because it's possible in
        // the future that wallet RPC methods might get or remain registered
        // when no wallets are loaded.
        throw JSONRPCError(
            RPC_METHOD_NOT_FOUND, "Method not found (wallet method is disabled because no wallet is loaded)");
    }
    throw JSONRPCError(RPC_WALLET_NOT_SPECIFIED,
        "Wallet file not specified (must request wallet RPC through /wallet/<filename> uri-path).");
}

void EnsureWalletIsUnlocked(CWallet * const pwallet)
{
    if (pwallet->IsLocked()) {
        throw JSONRPCError(RPC_WALLET_UNLOCK_NEEDED, "Error: Please enter the wallet passphrase with walletpassphrase first.");
    }

    if (pwallet->IsHDEnabled() && pwallet->fUnlockForStakingOnly)
        throw JSONRPCError(RPC_WALLET_UNLOCK_NEEDED, "Error: Wallet is unlocked for staking only.");
}

void WalletTxToJSON(const CWalletTx& wtx, UniValue& entry)
{
    int confirms = wtx.GetDepthInMainChain();
    entry.push_back(Pair("confirmations", confirms));
    if (wtx.IsCoinBase())
        entry.push_back(Pair("generated", true));
    if (confirms > 0)
    {
        entry.push_back(Pair("blockhash", wtx.hashBlock.GetHex()));
        entry.push_back(Pair("blockindex", wtx.nIndex));
        entry.push_back(Pair("blocktime", mapBlockIndex[wtx.hashBlock]->GetBlockTime()));
    } else {
        entry.push_back(Pair("trusted", wtx.IsTrusted()));
    }
    uint256 hash = wtx.GetHash();
    entry.push_back(Pair("txid", hash.GetHex()));
    UniValue conflicts(UniValue::VARR);
    for (const uint256& conflict : wtx.GetConflicts())
        conflicts.push_back(conflict.GetHex());
    entry.push_back(Pair("walletconflicts", conflicts));
    entry.push_back(Pair("time", wtx.GetTxTime()));
    entry.push_back(Pair("timereceived", (int64_t)wtx.nTimeReceived));

    // Add opt-in RBF status
    std::string rbfStatus = "no";
    if (confirms <= 0) {
        LOCK(mempool.cs);
        RBFTransactionState rbfState = IsRBFOptIn(*wtx.tx, mempool);
        if (rbfState == RBF_TRANSACTIONSTATE_UNKNOWN)
            rbfStatus = "unknown";
        else if (rbfState == RBF_TRANSACTIONSTATE_REPLACEABLE_BIP125)
            rbfStatus = "yes";
    }
    entry.push_back(Pair("bip125-replaceable", rbfStatus));

    for (const std::pair<std::string, std::string>& item : wtx.mapValue)
        entry.push_back(Pair(item.first, item.second));
}

std::string AccountFromValue(const UniValue& value)
{
    std::string strAccount = value.get_str();
    if (strAccount == "*")
        throw JSONRPCError(RPC_WALLET_INVALID_ACCOUNT_NAME, "Invalid account name");
    return strAccount;
}

UniValue getnewaddress(const JSONRPCRequest& request)
{
    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);
    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || request.params.size() > 2)
        throw std::runtime_error(
            "getnewaddress ( \"account\" \"address_type\" )\n"
            "\nReturns a new NIX address for receiving payments.\n"
            "If 'account' is specified (DEPRECATED), it is added to the address book \n"
            "so payments received with the address will be credited to 'account'.\n"
            "\nArguments:\n"
            "1. \"account\"        (string, optional) DEPRECATED. The account name for the address to be linked to. If not provided, the default account \"\" is used. It can also be set to the empty string \"\" to represent the default account. The account does not need to exist, it will be created if there is no account by the given name.\n"
            "2. \"address_type\"   (string, optional) The address type to use. Options are \"ghostnode\", \"p2sh-segwit(default)\", and \"bech32\". Default is set by -addresstype.\n"
            "\nResult:\n"
            "\"address\"    (string) The new nix address\n"
            "\nExamples:\n"
            + HelpExampleCli("getnewaddress", "")
            + HelpExampleRpc("getnewaddress", "")
        );

    LOCK2(cs_main, pwallet->cs_wallet);

    // Parse the account first so we don't generate a key if there's an error
    std::string strAccount;
    if (!request.params[0].isNull())
        strAccount = AccountFromValue(request.params[0]);

    OutputType output_type = g_address_type;
    if (!request.params[1].isNull()) {
        output_type = ParseOutputType(request.params[1].get_str(), g_address_type);
        if (output_type == OUTPUT_TYPE_NONE) {
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, strprintf("Unknown address type '%s'", request.params[1].get_str()));
        }
    }

    if (!pwallet->IsLocked()) {
        pwallet->TopUpKeyPool();
    }

    // Generate a new key that is added to wallet
    CPubKey newKey;
    if (!pwallet->GetKeyFromPool(newKey)) {
        throw JSONRPCError(RPC_WALLET_KEYPOOL_RAN_OUT, "Error: Keypool ran out, please call keypoolrefill first");
    }
    pwallet->LearnRelatedScripts(newKey, output_type);
    CTxDestination dest = GetDestinationForKey(newKey, output_type);

    pwallet->SetAddressBook(dest, strAccount, "receive");

    return EncodeDestination(dest);
}


CTxDestination GetAccountDestination(CWallet* const pwallet, std::string strAccount, bool bForceNew=false)
{
    CTxDestination dest;
    if (!pwallet->GetAccountDestination(dest, strAccount, bForceNew)) {
        throw JSONRPCError(RPC_WALLET_KEYPOOL_RAN_OUT, "Error: Keypool ran out, please call keypoolrefill first");
    }

    return dest;
}

UniValue getaccountaddress(const JSONRPCRequest& request)
{
    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);
    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || request.params.size() != 1)
        throw std::runtime_error(
            "getaccountaddress \"account\"\n"
            "\nDEPRECATED. Returns the current NIX address for receiving payments to this account.\n"
            "\nArguments:\n"
            "1. \"account\"       (string, required) The account name for the address. It can also be set to the empty string \"\" to represent the default account. The account does not need to exist, it will be created and a new address created  if there is no account by the given name.\n"
            "\nResult:\n"
            "\"address\"          (string) The account nix address\n"
            "\nExamples:\n"
            + HelpExampleCli("getaccountaddress", "")
            + HelpExampleCli("getaccountaddress", "\"\"")
            + HelpExampleCli("getaccountaddress", "\"myaccount\"")
            + HelpExampleRpc("getaccountaddress", "\"myaccount\"")
        );

    LOCK2(cs_main, pwallet->cs_wallet);

    // Parse the account first so we don't generate a key if there's an error
    std::string strAccount = AccountFromValue(request.params[0]);

    UniValue ret(UniValue::VSTR);

    ret = EncodeDestination(GetAccountDestination(pwallet, strAccount));
    return ret;
}


UniValue getrawchangeaddress(const JSONRPCRequest& request)
{
    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);
    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || request.params.size() > 1)
        throw std::runtime_error(
            "getrawchangeaddress ( \"address_type\" )\n"
            "\nReturns a new NIX address, for receiving change.\n"
            "This is for use with raw transactions, NOT normal use.\n"
            "\nArguments:\n"
            "1. \"address_type\"           (string, optional) The address type to use. Options are \"legacy\", \"p2sh-segwit\", and \"bech32\". Default is set by -changetype.\n"
            "\nResult:\n"
            "\"address\"    (string) The address\n"
            "\nExamples:\n"
            + HelpExampleCli("getrawchangeaddress", "")
            + HelpExampleRpc("getrawchangeaddress", "")
       );

    LOCK2(cs_main, pwallet->cs_wallet);

    if (!pwallet->IsLocked()) {
        pwallet->TopUpKeyPool();
    }

    OutputType output_type = g_change_type != OUTPUT_TYPE_NONE ? g_change_type : g_address_type;
    if (!request.params[0].isNull()) {
        output_type = ParseOutputType(request.params[0].get_str(), output_type);
        if (output_type == OUTPUT_TYPE_NONE) {
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, strprintf("Unknown address type '%s'", request.params[0].get_str()));
        }
    }

    CReserveKey reservekey(pwallet);
    CPubKey vchPubKey;
    if (!reservekey.GetReservedKey(vchPubKey, true))
        throw JSONRPCError(RPC_WALLET_KEYPOOL_RAN_OUT, "Error: Keypool ran out, please call keypoolrefill first");

    reservekey.KeepKey();

    pwallet->LearnRelatedScripts(vchPubKey, output_type);
    CTxDestination dest = GetDestinationForKey(vchPubKey, output_type);

    return EncodeDestination(dest);
}


UniValue setaccount(const JSONRPCRequest& request)
{
    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);
    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || request.params.size() < 1 || request.params.size() > 2)
        throw std::runtime_error(
            "setaccount \"address\" \"account\"\n"
            "\nDEPRECATED. Sets the account associated with the given address.\n"
            "\nArguments:\n"
            "1. \"address\"         (string, required) The nix address to be associated with an account.\n"
            "2. \"account\"         (string, required) The account to assign the address to.\n"
            "\nExamples:\n"
            + HelpExampleCli("setaccount", "\"1D1ZrZNe3JUo7ZycKEYQQiQAWd9y54F4XX\" \"tabby\"")
            + HelpExampleRpc("setaccount", "\"1D1ZrZNe3JUo7ZycKEYQQiQAWd9y54F4XX\", \"tabby\"")
        );

    LOCK2(cs_main, pwallet->cs_wallet);

    CTxDestination dest = DecodeDestination(request.params[0].get_str());
    if (!IsValidDestination(dest)) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid NIX address");
    }

    std::string strAccount;
    if (!request.params[1].isNull())
        strAccount = AccountFromValue(request.params[1]);

    // Only add the account if the address is yours.
    if (IsMine(*pwallet, dest)) {
        // Detect when changing the account of an address that is the 'unused current key' of another account:
        if (pwallet->mapAddressBook.count(dest)) {
            std::string strOldAccount = pwallet->mapAddressBook[dest].name;
            if (dest == GetAccountDestination(pwallet, strOldAccount)) {
                GetAccountDestination(pwallet, strOldAccount, true);
            }
        }
        pwallet->SetAddressBook(dest, strAccount, "receive");
    }
    else
        throw JSONRPCError(RPC_MISC_ERROR, "setaccount can only be used with own address");

    return NullUniValue;
}


UniValue getaccount(const JSONRPCRequest& request)
{
    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);
    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || request.params.size() != 1)
        throw std::runtime_error(
            "getaccount \"address\"\n"
            "\nDEPRECATED. Returns the account associated with the given address.\n"
            "\nArguments:\n"
            "1. \"address\"         (string, required) The nix address for account lookup.\n"
            "\nResult:\n"
            "\"accountname\"        (string) the account address\n"
            "\nExamples:\n"
            + HelpExampleCli("getaccount", "\"1D1ZrZNe3JUo7ZycKEYQQiQAWd9y54F4XX\"")
            + HelpExampleRpc("getaccount", "\"1D1ZrZNe3JUo7ZycKEYQQiQAWd9y54F4XX\"")
        );

    LOCK2(cs_main, pwallet->cs_wallet);

    CTxDestination dest = DecodeDestination(request.params[0].get_str());
    if (!IsValidDestination(dest)) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid NIX address");
    }

    std::string strAccount;
    std::map<CTxDestination, CAddressBookData>::iterator mi = pwallet->mapAddressBook.find(dest);
    if (mi != pwallet->mapAddressBook.end() && !(*mi).second.name.empty()) {
        strAccount = (*mi).second.name;
    }
    return strAccount;
}


UniValue getaddressesbyaccount(const JSONRPCRequest& request)
{
    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);
    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || request.params.size() != 1)
        throw std::runtime_error(
            "getaddressesbyaccount \"account\"\n"
            "\nDEPRECATED. Returns the list of addresses for the given account.\n"
            "\nArguments:\n"
            "1. \"account\"        (string, required) The account name.\n"
            "\nResult:\n"
            "[                     (json array of string)\n"
            "  \"address\"         (string) a nix address associated with the given account\n"
            "  ,...\n"
            "]\n"
            "\nExamples:\n"
            + HelpExampleCli("getaddressesbyaccount", "\"tabby\"")
            + HelpExampleRpc("getaddressesbyaccount", "\"tabby\"")
        );

    LOCK2(cs_main, pwallet->cs_wallet);

    std::string strAccount = AccountFromValue(request.params[0]);

    // Find all addresses that have the given account
    UniValue ret(UniValue::VARR);
    for (const std::pair<CTxDestination, CAddressBookData>& item : pwallet->mapAddressBook) {
        const CTxDestination& dest = item.first;
        const std::string& strName = item.second.name;
        if (strName == strAccount) {
            ret.push_back(EncodeDestination(dest));
        }
    }
    return ret;
}

UniValue getfeeforamount(const JSONRPCRequest& request)
{
    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);
    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }


    if (request.fHelp || request.params.size() != 2)
        throw std::runtime_error(
            "getfeeforamount \"amount\" \"address\"\n"
            "\n. Returns the fee needed for the amount needed to send.\n"
            "\nArguments:\n"
            "1. \"amount\"        (int, required) The amount you want for fee calculation.\n"
            "2. \"address\"       (string, required) The address you want to send to for fee calculation.\n"
            "\nResult:\n"
            "\"fee\"                   (json string of fee)\n"
            "\nExamples:\n"
            + HelpExampleCli("getfeeforamount", "\"400\" \"ZM72Sfpbz1BPpXFHz9m3CdqATR44Jvaydd\""));


    // Amount
    CAmount nAmount = AmountFromValue(request.params[0]);

    CTxDestination destination = DecodeDestination(request.params[1].get_str());

    if (!IsValidDestination(destination)) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid address");
    }
    CScript dest = GetScriptForDestination(destination);

    CAmount curBalance = pwallet->GetBalance();

    if (nAmount <= 0)
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid amount");

    if (nAmount > curBalance)
        throw JSONRPCError(RPC_WALLET_INSUFFICIENT_FUNDS, "Insufficient funds");

    // Create dummy with correct value
    CAmount nFeeRequired;
    std::string strError;
    std::vector<CRecipient> vecSend;
    int nChangePosRet = -1;
    CRecipient recipient = {dest, nAmount, false};
    vecSend.push_back(recipient);
    CCoinControl coin_control;
    if (!pwallet->GetFeeForTransaction(vecSend, nFeeRequired, nChangePosRet, strError, coin_control)) {
        if (nAmount + nFeeRequired > curBalance)
            strError = strprintf("Error: This transaction requires a transaction fee of at least %s", FormatMoney(nFeeRequired));
        throw JSONRPCError(RPC_WALLET_ERROR, strError);
    }

    return nFeeRequired;

}
static void SendMoney(CWallet * const pwallet, const CTxDestination &address, CAmount nValue, bool fSubtractFeeFromAmount, CWalletTx& wtxNew, const CCoinControl& coin_control/*, CScript ghostKey*/)
{
    CAmount curBalance = pwallet->GetBalance();
    LogPrintf("\nCurrent balance: %lf, nValue: %lf \n", curBalance, nValue);

    // Check amount
    if (nValue <= 0)
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid amount");

    if (nValue > curBalance)
        throw JSONRPCError(RPC_WALLET_INSUFFICIENT_FUNDS, "Insufficient funds");

    if (pwallet->GetBroadcastTransactions() && !g_connman) {
        throw JSONRPCError(RPC_CLIENT_P2P_DISABLED, "Error: Peer-to-peer functionality missing or disabled");
    }

    // Parse Bitcoin address
    CScript scriptPubKey = GetScriptForDestination(address);

    // Create and send the transaction
    CReserveKey reservekey(pwallet);
    CAmount nFeeRequired;
    std::string strError;
    std::vector<CRecipient> vecSend;
    int nChangePosRet = -1;
    CRecipient recipient = {scriptPubKey, nValue, fSubtractFeeFromAmount};
    vecSend.push_back(recipient);

    /*
    //include public ghost key in transaction if using a ghost address
    if(!ghostKey.empty()){
        CRecipient recipientGhost = {ghostKey, 0, fSubtractFeeFromAmount};
        vecSend.push_back(recipientGhost);
    }
    */

    if (!pwallet->CreateTransaction(vecSend, wtxNew, reservekey, nFeeRequired, nChangePosRet, strError, coin_control)) {
        if (!fSubtractFeeFromAmount && nValue + nFeeRequired > curBalance)
            strError = strprintf("Error: This transaction requires a transaction fee of at least %s", FormatMoney(nFeeRequired));
        throw JSONRPCError(RPC_WALLET_ERROR, strError);
    }
    CValidationState state;
    if (!pwallet->CommitTransaction(wtxNew, reservekey, g_connman.get(), state)) {
        strError = strprintf("Error: The transaction was rejected! Reason given: %s", state.GetRejectReason());
        throw JSONRPCError(RPC_WALLET_ERROR, strError);
    }
}

UniValue sendtoaddress(const JSONRPCRequest& request)
{
    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);
    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || request.params.size() < 2 || request.params.size() > 8)
        throw std::runtime_error(
            "sendtoaddress \"address\" amount ( \"comment\" \"comment_to\" subtractfeefromamount replaceable conf_target \"estimate_mode\")\n"
            "\nSend an amount to a given address.\n"
            + HelpRequiringPassphrase(pwallet) +
            "\nArguments:\n"
            "1. \"address\"            (string, required) The nix address to send to.\n"
            "2. \"amount\"             (numeric or string, required) The amount in " + CURRENCY_UNIT + " to send. eg 0.1\n"
            "3. \"comment\"            (string, optional) A comment used to store what the transaction is for. \n"
            "                             This is not part of the transaction, just kept in your wallet.\n"
            "4. \"comment_to\"         (string, optional) A comment to store the name of the person or organization \n"
            "                             to which you're sending the transaction. This is not part of the \n"
            "                             transaction, just kept in your wallet.\n"
            "5. subtractfeefromamount  (boolean, optional, default=false) The fee will be deducted from the amount being sent.\n"
            "                             The recipient will receive less nix than you enter in the amount field.\n"
            "6. replaceable            (boolean, optional) Allow this transaction to be replaced by a transaction with higher fees via BIP 125\n"
            "7. conf_target            (numeric, optional) Confirmation target (in blocks)\n"
            "8. \"estimate_mode\"      (string, optional, default=UNSET) The fee estimate mode, must be one of:\n"
            "       \"UNSET\"\n"
            "       \"ECONOMICAL\"\n"
            "       \"CONSERVATIVE\"\n"
            "\nResult:\n"
            "\"txid\"                  (string) The transaction id.\n"
            "\nExamples:\n"
            + HelpExampleCli("sendtoaddress", "\"1M72Sfpbz1BPpXFHz9m3CdqATR44Jvaydd\" 0.1")
            + HelpExampleCli("sendtoaddress", "\"1M72Sfpbz1BPpXFHz9m3CdqATR44Jvaydd\" 0.1 \"donation\" \"seans outpost\"")
            + HelpExampleCli("sendtoaddress", "\"1M72Sfpbz1BPpXFHz9m3CdqATR44Jvaydd\" 0.1 \"\" \"\" true")
            + HelpExampleRpc("sendtoaddress", "\"1M72Sfpbz1BPpXFHz9m3CdqATR44Jvaydd\", 0.1, \"donation\", \"seans outpost\"")
        );

    ObserveSafeMode();

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pwallet->BlockUntilSyncedToCurrentChain();

    LOCK2(cs_main, pwallet->cs_wallet);

    CTxDestination dest = DecodeDestination(request.params[0].get_str());

    if (!IsValidDestination(dest)) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid address");
    }

    // Amount
    CAmount nAmount = AmountFromValue(request.params[1]);
    if (nAmount <= 0)
        throw JSONRPCError(RPC_TYPE_ERROR, "Invalid amount for send");

    // Wallet comments
    CWalletTx wtx;
    if (!request.params[2].isNull() && !request.params[2].get_str().empty())
        wtx.mapValue["comment"] = request.params[2].get_str();
    if (!request.params[3].isNull() && !request.params[3].get_str().empty())
        wtx.mapValue["to"]      = request.params[3].get_str();

    bool fSubtractFeeFromAmount = false;
    if (!request.params[4].isNull()) {
        fSubtractFeeFromAmount = request.params[4].get_bool();
    }

    CCoinControl coin_control;
    if (!request.params[5].isNull()) {
        coin_control.signalRbf = request.params[5].get_bool();
    }

    if (!request.params[6].isNull()) {
        coin_control.m_confirm_target = ParseConfirmTarget(request.params[6]);
    }

    if (!request.params[7].isNull()) {
        if (!FeeModeFromString(request.params[7].get_str(), coin_control.m_fee_mode)) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid estimate_mode parameter");
        }
    }


    EnsureWalletIsUnlocked(pwallet);

    SendMoney(pwallet, dest, nAmount, fSubtractFeeFromAmount, wtx, coin_control/*, ghostKey*/);

    return wtx.GetHash().GetHex();
}

UniValue leasestaking(const JSONRPCRequest& request)
{
    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);
    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || request.params.size() < 2 || request.params.size() > 8)
        throw std::runtime_error(
            "leasestaking \"address\" amount ( \"comment\" \"comment_to\" subtractfeefromamount replaceable conf_target \"estimate_mode\")\n"
            "\nLease an amount of nix to a certain address to stake.\n"
            + HelpRequiringPassphrase(pwallet) +
            "\nArguments:\n"
            "1. \"lease address\"                    (string, required) The nix address to lease stakes to.\n"
            "2. \"amount\"                              (numeric or string, required) The amount in " + CURRENCY_UNIT + " to send. eg 0.1\n"
            "3. \"label\"                              (string, optional) The contract label\n"
            "4. \"fee percent\"                         (numeric, optional) The percentage to allow delegator to take. eg 11.9 (11.9%)\n"
            "5. \"lease percent reward address\"     (string, optional) The nix address to force lease fee stakes to.\n"

            "6. \"comment\"            (string, optional) A comment used to store what the transaction is for. \n"
            "                             This is not part of the transaction, just kept in your wallet.\n"
            "7. \"comment_to\"         (string, optional) A comment to store the name of the person or organization \n"
            "                             to which you're sending the transaction. This is not part of the \n"
            "                             transaction, just kept in your wallet.\n"
            "8. subtractfeefromamount  (boolean, optional, default=false) The fee will be deducted from the amount being sent.\n"
            "                             The recipient will receive less nix than you enter in the amount field.\n"
            "9. replaceable            (boolean, optional) Allow this transaction to be replaced by a transaction with higher fees via BIP 125\n"
            "10. conf_target            (numeric, optional) Confirmation target (in blocks)\n"
            "11. \"estimate_mode\"      (string, optional, default=UNSET) The fee estimate mode, must be one of:\n"
            "       \"UNSET\"\n"
            "       \"ECONOMICAL\"\n"
            "       \"CONSERVATIVE\"\n"
            "\nResult:\n"
            "\"txid\"                  (string) The transaction id.\n"
            "\nExamples:\n"
            + HelpExampleCli("leasestaking", "\"Nf72Sfpbz1BPpXFHz9m3CdqATR44Jvaydd\" 150")
            + HelpExampleCli("leasestaking", "\"Nf72Sfpbz1BPpXFHz9m3CdqATR44Jvaydd\" 150 11.9 \"NG72Sfpbz1BLpXFHz9m3CdqATR44JDaydd\"")
        );

    ObserveSafeMode();

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pwallet->BlockUntilSyncedToCurrentChain();

    if(chainActive.Height() < Params().GetConsensus().nStartGhostFeeDistribution)
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Cannot create lease contract until block 114,000");

    LOCK2(cs_main, pwallet->cs_wallet);

    CTxDestination dest;

    dest = DecodeDestination(request.params[0].get_str());

    if (!IsValidDestination(dest)) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid address");
    }

    // Amount
    CAmount nAmount = AmountFromValue(request.params[1]);
    if (nAmount <= 0)
        throw JSONRPCError(RPC_TYPE_ERROR, "Invalid amount for send");

    CWalletTx wtx;

    bool fSubtractFeeFromAmount = false;
    if (!request.params[5].isNull()) {
        fSubtractFeeFromAmount = request.params[5].get_bool();
    }




    EnsureWalletIsUnlocked(pwallet);

    CAmount curBalance = pwallet->GetBalance();

    // Check amount
    if (nAmount <= 0)
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid amount");

    if (nAmount > curBalance)
        throw JSONRPCError(RPC_WALLET_INSUFFICIENT_FUNDS, "Insufficient funds");

    if (pwallet->GetBroadcastTransactions() && !g_connman) {
        throw JSONRPCError(RPC_CLIENT_P2P_DISABLED, "Error: Peer-to-peer functionality missing or disabled");
    }

    // Parse coldstaking address
    CScript delegateScript = GetScriptForDestination(dest);


    if (delegateScript.IsPayToPublicKeyHash())
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid lease key");


    OutputType outType = OUTPUT_TYPE_DEFAULT;

    if(delegateScript.IsPayToScriptHash())
        outType = OUTPUT_TYPE_P2SH_SEGWIT;
    else if(delegateScript.IsPayToWitnessKeyHash())
        outType = OUTPUT_TYPE_BECH32;
    else{
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid lease key");
    }


    // Generate a new key that is added to wallet
    CPubKey newKey;
    if (!pwallet->GetKeyFromPool(newKey)) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Cannot get key from pool");
    }

    pwallet->LearnRelatedScripts(newKey, outType);
    CTxDestination returnAddr = GetDestinationForKey(newKey, outType);

    CScript scriptPubKeyKernel = GetScriptForDestination(returnAddr);
    //set up contract
    CScript script = CScript() << OP_ISCOINSTAKE << OP_IF;
    //cold stake address
    script += delegateScript;
    script << OP_ELSE;
    //local wallet address
    script += scriptPubKeyKernel;
    script << OP_ENDIF;

    // Fee
    int64_t nFeePercent = 0;
    if (!request.params[3].isNull()){
        nFeePercent = int64_t(AmountFromValue(request.params[3])/1000000);
        if(nFeePercent > 10000 || nFeePercent < 0){
            throw JSONRPCError(RPC_INVALID_PARAMETER, "nFeePercent too large. Must be between 0 and 100");;
        }
        script << nFeePercent;
        script << OP_DROP;
    }
    // Reward address
    if (!request.params[3].isNull() && !request.params[4].get_str().empty()){
        if(!IsValidDestination(DecodeDestination(request.params[4].get_str())))
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid reward address");
        // Parse coldstaking fee reward address
        // Take only txdestination, leave out hash160 and equal when including in script
        CScript delegateScriptRewardTemp = GetScriptForDestination(DecodeDestination(request.params[4].get_str()));
        if (delegateScriptRewardTemp.IsPayToPublicKeyHash())
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid delagate key");

        //Returns false if not coldstake or p2sh script
        CScriptID destReward;
        WitnessV0KeyHash witness_ID;
        witness_ID.SetNull();
        if (!ExtractStakingKeyID(delegateScriptRewardTemp, destReward, witness_ID))
            throw JSONRPCError(RPC_INVALID_PARAMETER, "ExtractStakingKeyID return false");
        if(witness_ID.IsNull())
            script << ToByteVector(destReward);
        else
            script << ToByteVector(witness_ID);
        script << OP_DROP;
    }

    scriptPubKeyKernel = script;

    /*
    // Check if contract allows fee payouts
    int64_t feeOut = 0;
    CAmount feeAmount = 0;
    CAmount nReward = 2.24 * COIN;
    CAmount amount = 100 * COIN;
    if(GetCoinstakeScriptFee(scriptPubKeyKernel, feeOut)){
        double feePercent = (double)feeOut;
        if(feeOut > 10000 || feeOut < 0){
            return false;
        }
        feePercent /= 100;
        amount += nReward * (double)((100.0 - feePercent)/100.0);
        feeAmount = nReward * ((feePercent)/100);
        LogPrintf("\nScriptFee\n");
    }


    CScript scriptOut;
    if(GetCoinstakeScriptFeeRewardAddress(scriptPubKeyKernel, scriptOut)){
        LogPrintf("\nFeeRewardAddress\n");
    }

    CTxDestination destination;
    ExtractDestination(scriptOut, destination);
    CBitcoinAddress btcTest(destination);

    LogPrintf("\nRunning Test: feeAmount=%llf, nReward=%llf, amount=%llf, destination=%s", feeAmount, nReward, amount, btcTest.ToString());
    return "null";
    */

    // Create and send the transaction
    CReserveKey reservekey(pwallet);
    CAmount nFeeRequired;
    std::string strError;
    std::vector<CRecipient> vecSend;
    int nChangePosRet = -1;
    CRecipient recipient = {scriptPubKeyKernel, nAmount, fSubtractFeeFromAmount};
    vecSend.push_back(recipient);

    CCoinControl coin_control;

    if (!pwallet->CreateTransaction(vecSend, wtx, reservekey, nFeeRequired, nChangePosRet, strError, coin_control)) {
        if (!fSubtractFeeFromAmount && nAmount + nFeeRequired > curBalance)
            strError = strprintf("Error: This transaction requires a transaction fee of at least %s", FormatMoney(nFeeRequired));
        throw JSONRPCError(RPC_WALLET_ERROR, strError);
    }
    CValidationState state;
    if (!pwallet->CommitTransaction(wtx, reservekey, g_connman.get(), state)) {
        strError = strprintf("Error: The transaction was rejected! Reason given: %s", state.GetRejectReason());
        throw JSONRPCError(RPC_WALLET_ERROR, strError);
    }

    // label the address at the end to ensure tx went ok
    if(!request.params[2].isNull()){
        pwallet->SetAddressBook(returnAddr, request.params[2].get_str(), "receive");
    }

    // lock the output
    int out_index = 0;
    for(auto &tx :wtx.tx->vout){
        if(tx.scriptPubKey.IsPayToScriptHash_CS() || tx.scriptPubKey.IsPayToWitnessKeyHash_CS()){
            COutPoint lposOut(wtx.GetHash(), out_index);
            pwallet->LockCoin(lposOut);
        }
        out_index++;
    }

    return wtx.GetHash().GetHex();
}

UniValue getleasestakinglist(const JSONRPCRequest& request)
{
    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);
    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || request.params.size() > 0 )
        throw std::runtime_error(
            "getleasestakinglist \n"
            "\nGet list of current LPoS contracts in wallet.\n"
            + HelpRequiringPassphrase(pwallet));


    LOCK2(cs_main, pwallet->cs_wallet);

    std::map<std::string, std::vector<COutput> > mapCoins;

    // push all coins from all addresses into mapping
    for (auto& group : pwallet->ListCoins()) {
        auto& resultGroup = mapCoins[EncodeDestination(group.first)];
        for (auto& coin : group.second) {
            resultGroup.emplace_back(std::move(coin));
        }
    }

    UniValue lposContracts(UniValue::VOBJ);

    // unlock all previous contracts
    for(int i = 0; i < pwallet->activeContracts.size(); i++){
        COutPoint point = pwallet->activeContracts[i];
        pwallet->UnlockCoin(point);
    }

    pwallet->activeContracts.clear();

    int contractAmount = 0;
    for (const std::pair<std::string, std::vector<COutput>>& coins : mapCoins) {
        CAmount nSum = 0;
        for (const COutput& out : coins.second) {
            nSum = out.tx->tx->vout[out.i].nValue;
            //skip spent coins
            if(pwallet->IsSpent(out.tx->tx->vout[out.i].GetHash(), out.i)) continue;

            // address
            CTxDestination ownerDest;
            if(out.tx->tx->vout[out.i].scriptPubKey.IsPayToScriptHash_CS()
                    || out.tx->tx->vout[out.i].scriptPubKey.IsPayToWitnessKeyHash_CS()){
                if(ExtractDestination(out.tx->tx->vout[out.i].scriptPubKey, ownerDest))
                {
                    CScript ownerScript;
                    CScript delegateScript;
                    int64_t feeAmount;
                    CScript feeRewardScript;
                    CScriptID hash;

                    if (out.tx->tx->vout[out.i].scriptPubKey.IsPayToWitnessKeyHash_CS()){
                        //p2wkh
                        GetNonCoinstakeScriptPath(out.tx->tx->vout[out.i].scriptPubKey, ownerScript);
                        hash =  CScriptID(ownerScript);
                    }
                    else
                        hash = boost::get<CScriptID>(ownerDest);

                    if(pwallet->HaveCScript(hash)){
                        GetCoinstakeScriptPath(out.tx->tx->vout[out.i].scriptPubKey, delegateScript);
                        bool hasFee = GetCoinstakeScriptFee(out.tx->tx->vout[out.i].scriptPubKey, feeAmount);
                        GetCoinstakeScriptFeeRewardAddress(out.tx->tx->vout[out.i].scriptPubKey, feeRewardScript);

                        CBitcoinAddress addr1(ownerDest);

                        CTxDestination delegateDest;
                        ExtractDestination(delegateScript, delegateDest);
                        CBitcoinAddress addr2(delegateDest);

                        CTxDestination rewardFeeDest;
                        ExtractDestination(feeRewardScript, rewardFeeDest);
                        CBitcoinAddress addr3(rewardFeeDest);

                        if(!hasFee)
                            feeAmount = 0;

                        std::string ownerAddrString = addr1.ToString();
                        std::string leaseAddress = addr2.ToString();
                        std::string rewardAddress = addr3.ToString();

                        if(out.tx->tx->vout[out.i].scriptPubKey.IsPayToWitnessKeyHash_CS()){
                            ownerAddrString = EncodeDestination(ownerDest, true);
                            leaseAddress = EncodeDestination(delegateDest, true);
                            rewardAddress = EncodeDestination(rewardFeeDest, true);
                        }

                        if(pwallet->mapAddressBook.find(ownerDest) != pwallet->mapAddressBook.end())
                        {
                            if(pwallet->mapAddressBook[ownerDest].name != "")
                                ownerAddrString = pwallet->mapAddressBook[ownerDest].name;
                        }

                        if(!hasFee)
                            rewardAddress = "N/A";

                        UniValue contract(UniValue::VOBJ);
                        contract.pushKV("my_address", ownerAddrString);
                        contract.pushKV("lease_address", leaseAddress);
                        contract.pushKV("fee", std::to_string((double)feeAmount/100.00));
                        contract.pushKV("reward_fee_address", rewardAddress);
                        contract.pushKV("amount", std::to_string(nSum));
                        contract.pushKV("tx_hash", out.tx->tx->GetHash().GetHex());
                        contract.pushKV("tx_index", std::to_string(out.i));

                        lposContracts.pushKV("contract " + std::to_string(contractAmount), contract);
                        contractAmount++;

                        COutPoint point(out.tx->GetHash(), out.i);
                        pwallet->LockCoin(point);
                        pwallet->activeContracts.push_back(point);

                    }
                }
            }
        }
    }

    return lposContracts;
}

UniValue cancelstakingcontract(const JSONRPCRequest& request){

    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);
    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || request.params.size() != 3)
        throw std::runtime_error(
            "cancelleaststakingcontract tx_hash tx_index\n"
            "\nCancel a contract in this wallet using the tx hash and tx index indentifiers.\n"
            + HelpRequiringPassphrase(pwallet) +
            "\nArguments:\n"
            "1. \"tx_hash\"                    (string, required) The transaction hash of the contract you are trying to cancel.\n"
            "2. \"tx_index\"                   (numeric or string, required) The index of the transaction. eg 1\n"
            "3. \"amount\"                     (numeric or string, required) The amount of the transaction. eg 10\n"
            "\nResult:\n"
            "\"txid\"                  (string) The transaction id of the canceled contract.\n"
            "\nExamples:\n"
            + HelpExampleCli("cancelleaststakingcontract", "98c74c91d69511167de6c07f21b1c6449786a53e8df2892772ba0355abd01b6d 0 10")
        );

    std::string hashStr = request.params[0].get_str();

    const uint256 hash = uint256S(hashStr);
    std::string txIndexStr = request.params[1].get_str();
    stringstream convert(txIndexStr);
    int x = 0;
    convert >> x;
    const int index = x;
    CCoinControl ctrl;
    ctrl.UnSelectAll();
    COutPoint point(hash, index);
    ctrl.Select(point);
    CAmount totalAmount = AmountFromValue(request.params[2]);
    pwallet->UnlockCoin(point);

    if (!pwallet->IsLocked()) {
        pwallet->TopUpKeyPool();
    }

    std::string strError;
    // Generate a new key that is added to wallet
    CPubKey newKey;
    if (!pwallet->GetKeyFromPool(newKey)) {
        strError = strprintf("Error: GetKeyFromPool\n");
        throw JSONRPCError(RPC_WALLET_ERROR, strError);    }

    pwallet->LearnRelatedScripts(newKey, g_address_type);
    CTxDestination dest = GetDestinationForKey(newKey, g_address_type);

    CScript scriptPubKey = GetScriptForDestination(dest);


    // Create and send the transaction
    CReserveKey reservekey(pwallet);
    CAmount nFeeRequired;
    std::vector<CRecipient> vecSend;
    int nChangePosRet = -1;
    CRecipient recipient = {scriptPubKey, totalAmount, true};
    vecSend.push_back(recipient);

    CWalletTx wtx;

    if (!pwallet->CreateTransaction(vecSend, wtx, reservekey, nFeeRequired, nChangePosRet, strError, ctrl)) {
        strError = strprintf("Error: Create transaction was rejected! Reason given: %s", strError);
        throw JSONRPCError(RPC_WALLET_ERROR, strError);
    }


    /**************************************************************************************************************
         **************************************************************************************************************
         **************************************************************************************************************
         *  End send dialog  */

    CValidationState state;
    if (!pwallet->CommitTransaction(wtx, reservekey, g_connman.get(), state)) {
        strError = strprintf("Error: Commit Transaction was rejected! Reason given: %s", state.GetRejectReason());
        throw JSONRPCError(RPC_WALLET_ERROR, strError);
    }

    return wtx.GetHash().GetHex();
}

UniValue listaddressgroupings(const JSONRPCRequest& request)
{
    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);
    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || request.params.size() != 0)
        throw std::runtime_error(
            "listaddressgroupings\n"
            "\nLists groups of addresses which have had their common ownership\n"
            "made public by common use as inputs or as the resulting change\n"
            "in past transactions\n"
            "\nResult:\n"
            "[\n"
            "  [\n"
            "    [\n"
            "      \"address\",            (string) The nix address\n"
            "      amount,                 (numeric) The amount in " + CURRENCY_UNIT + "\n"
            "      \"account\"             (string, optional) DEPRECATED. The account\n"
            "    ]\n"
            "    ,...\n"
            "  ]\n"
            "  ,...\n"
            "]\n"
            "\nExamples:\n"
            + HelpExampleCli("listaddressgroupings", "")
            + HelpExampleRpc("listaddressgroupings", "")
        );

    ObserveSafeMode();

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pwallet->BlockUntilSyncedToCurrentChain();

    LOCK2(cs_main, pwallet->cs_wallet);

    UniValue jsonGroupings(UniValue::VARR);
    std::map<CTxDestination, CAmount> balances = pwallet->GetAddressBalances();
    for (const std::set<CTxDestination>& grouping : pwallet->GetAddressGroupings()) {
        UniValue jsonGrouping(UniValue::VARR);
        for (const CTxDestination& address : grouping)
        {
            UniValue addressInfo(UniValue::VARR);
            addressInfo.push_back(EncodeDestination(address));
            addressInfo.push_back(ValueFromAmount(balances[address]));
            {
                if (pwallet->mapAddressBook.find(address) != pwallet->mapAddressBook.end()) {
                    addressInfo.push_back(pwallet->mapAddressBook.find(address)->second.name);
                }
            }
            jsonGrouping.push_back(addressInfo);
        }
        jsonGroupings.push_back(jsonGrouping);
    }
    return jsonGroupings;
}

UniValue signmessage(const JSONRPCRequest& request)
{
    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);
    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || request.params.size() != 2)
        throw std::runtime_error(
            "signmessage \"address\" \"message\"\n"
            "\nSign a message with the private key of an address"
            + HelpRequiringPassphrase(pwallet) + "\n"
            "\nArguments:\n"
            "1. \"address\"         (string, required) The nix address to use for the private key.\n"
            "2. \"message\"         (string, required) The message to create a signature of.\n"
            "\nResult:\n"
            "\"signature\"          (string) The signature of the message encoded in base 64\n"
            "\nExamples:\n"
            "\nUnlock the wallet for 30 seconds\n"
            + HelpExampleCli("walletpassphrase", "\"mypassphrase\" 30") +
            "\nCreate the signature\n"
            + HelpExampleCli("signmessage", "\"1D1ZrZNe3JUo7ZycKEYQQiQAWd9y54F4XX\" \"my message\"") +
            "\nVerify the signature\n"
            + HelpExampleCli("verifymessage", "\"1D1ZrZNe3JUo7ZycKEYQQiQAWd9y54F4XX\" \"signature\" \"my message\"") +
            "\nAs json rpc\n"
            + HelpExampleRpc("signmessage", "\"1D1ZrZNe3JUo7ZycKEYQQiQAWd9y54F4XX\", \"my message\"")
        );

    LOCK2(cs_main, pwallet->cs_wallet);

    EnsureWalletIsUnlocked(pwallet);

    std::string strAddress = request.params[0].get_str();
    std::string strMessage = request.params[1].get_str();

    CTxDestination dest = DecodeDestination(strAddress);

    if (!IsValidDestination(dest)) {
        throw JSONRPCError(RPC_TYPE_ERROR, "Invalid address");
    }

    const CKeyID keyID = GetKeyForDestination(*pwallet, dest);
    //const CKeyID *keyID = boost::get<CKeyID>(&dest);
    if (keyID.IsNull()) {
        throw JSONRPCError(RPC_TYPE_ERROR, "Address does not refer to key");
    }

    CKey key;
    if (!pwallet->GetKey(keyID, key)) {
        throw JSONRPCError(RPC_WALLET_ERROR, "Private key not available");
    }

    CHashWriter ss(SER_GETHASH, 0);
    ss << strMessageMagic;
    ss << strMessage;

    std::vector<unsigned char> vchSig;
    if (!key.SignCompact(ss.GetHash(), vchSig))
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Sign failed");

    return EncodeBase64(vchSig.data(), vchSig.size());
}

UniValue getreceivedbyaddress(const JSONRPCRequest& request)
{
    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);
    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || request.params.size() < 1 || request.params.size() > 2)
        throw std::runtime_error(
            "getreceivedbyaddress \"address\" ( minconf )\n"
            "\nReturns the total amount received by the given address in transactions with at least minconf confirmations.\n"
            "\nArguments:\n"
            "1. \"address\"         (string, required) The nix address for transactions.\n"
            "2. minconf             (numeric, optional, default=1) Only include transactions confirmed at least this many times.\n"
            "\nResult:\n"
            "amount   (numeric) The total amount in " + CURRENCY_UNIT + " received at this address.\n"
            "\nExamples:\n"
            "\nThe amount from transactions with at least 1 confirmation\n"
            + HelpExampleCli("getreceivedbyaddress", "\"1D1ZrZNe3JUo7ZycKEYQQiQAWd9y54F4XX\"") +
            "\nThe amount including unconfirmed transactions, zero confirmations\n"
            + HelpExampleCli("getreceivedbyaddress", "\"1D1ZrZNe3JUo7ZycKEYQQiQAWd9y54F4XX\" 0") +
            "\nThe amount with at least 6 confirmations\n"
            + HelpExampleCli("getreceivedbyaddress", "\"1D1ZrZNe3JUo7ZycKEYQQiQAWd9y54F4XX\" 6") +
            "\nAs a json rpc call\n"
            + HelpExampleRpc("getreceivedbyaddress", "\"1D1ZrZNe3JUo7ZycKEYQQiQAWd9y54F4XX\", 6")
       );

    ObserveSafeMode();

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pwallet->BlockUntilSyncedToCurrentChain();

    LOCK2(cs_main, pwallet->cs_wallet);

    // Bitcoin address
    CTxDestination dest = DecodeDestination(request.params[0].get_str());
    if (!IsValidDestination(dest)) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid NIX address");
    }
    CScript scriptPubKey = GetScriptForDestination(dest);
    if (!IsMine(*pwallet, scriptPubKey)) {
        throw JSONRPCError(RPC_WALLET_ERROR, "Address not found in wallet");
    }

    // Minimum confirmations
    int nMinDepth = 1;
    if (!request.params[1].isNull())
        nMinDepth = request.params[1].get_int();

    // Tally
    CAmount nAmount = 0;
    for (const std::pair<uint256, CWalletTx>& pairWtx : pwallet->mapWallet) {
        const CWalletTx& wtx = pairWtx.second;
        if (wtx.IsCoinBase() || !CheckFinalTx(*wtx.tx))
            continue;

        for (const CTxOut& txout : wtx.tx->vout)
            if (txout.scriptPubKey == scriptPubKey)
                if (wtx.GetDepthInMainChain() >= nMinDepth)
                    nAmount += txout.nValue;
    }

    return  ValueFromAmount(nAmount);
}


UniValue getreceivedbyaccount(const JSONRPCRequest& request)
{
    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);
    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || request.params.size() < 1 || request.params.size() > 2)
        throw std::runtime_error(
            "getreceivedbyaccount \"account\" ( minconf )\n"
            "\nDEPRECATED. Returns the total amount received by addresses with <account> in transactions with at least [minconf] confirmations.\n"
            "\nArguments:\n"
            "1. \"account\"      (string, required) The selected account, may be the default account using \"\".\n"
            "2. minconf          (numeric, optional, default=1) Only include transactions confirmed at least this many times.\n"
            "\nResult:\n"
            "amount              (numeric) The total amount in " + CURRENCY_UNIT + " received for this account.\n"
            "\nExamples:\n"
            "\nAmount received by the default account with at least 1 confirmation\n"
            + HelpExampleCli("getreceivedbyaccount", "\"\"") +
            "\nAmount received at the tabby account including unconfirmed amounts with zero confirmations\n"
            + HelpExampleCli("getreceivedbyaccount", "\"tabby\" 0") +
            "\nThe amount with at least 6 confirmations\n"
            + HelpExampleCli("getreceivedbyaccount", "\"tabby\" 6") +
            "\nAs a json rpc call\n"
            + HelpExampleRpc("getreceivedbyaccount", "\"tabby\", 6")
        );

    ObserveSafeMode();

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pwallet->BlockUntilSyncedToCurrentChain();

    LOCK2(cs_main, pwallet->cs_wallet);

    // Minimum confirmations
    int nMinDepth = 1;
    if (!request.params[1].isNull())
        nMinDepth = request.params[1].get_int();

    // Get the set of pub keys assigned to account
    std::string strAccount = AccountFromValue(request.params[0]);
    std::set<CTxDestination> setAddress = pwallet->GetAccountAddresses(strAccount);

    // Tally
    CAmount nAmount = 0;
    for (const std::pair<uint256, CWalletTx>& pairWtx : pwallet->mapWallet) {
        const CWalletTx& wtx = pairWtx.second;
        if (wtx.IsCoinBase() || !CheckFinalTx(*wtx.tx))
            continue;

        for (const CTxOut& txout : wtx.tx->vout)
        {
            CTxDestination address;
            if (ExtractDestination(txout.scriptPubKey, address) && IsMine(*pwallet, address) && setAddress.count(address)) {
                if (wtx.GetDepthInMainChain() >= nMinDepth)
                    nAmount += txout.nValue;
            }
        }
    }

    return ValueFromAmount(nAmount);
}


UniValue getbalance(const JSONRPCRequest& request)
{
    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);
    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || request.params.size() > 3)
        throw std::runtime_error(
            "getbalance ( \"account\" minconf include_watchonly )\n"
            "\nIf account is not specified, returns the server's total available balance.\n"
            "The available balance is what the wallet considers currently spendable, and is\n"
            "thus affected by options which limit spendability such as -spendzeroconfchange.\n"
            "If account is specified (DEPRECATED), returns the balance in the account.\n"
            "Note that the account \"\" is not the same as leaving the parameter out.\n"
            "The server total may be different to the balance in the default \"\" account.\n"
            "\nArguments:\n"
            "1. \"account\"         (string, optional) DEPRECATED. The account string may be given as a\n"
            "                     specific account name to find the balance associated with wallet keys in\n"
            "                     a named account, or as the empty string (\"\") to find the balance\n"
            "                     associated with wallet keys not in any named account, or as \"*\" to find\n"
            "                     the balance associated with all wallet keys regardless of account.\n"
            "                     When this option is specified, it calculates the balance in a different\n"
            "                     way than when it is not specified, and which can count spends twice when\n"
            "                     there are conflicting pending transactions (such as those created by\n"
            "                     the bumpfee command), temporarily resulting in low or even negative\n"
            "                     balances. In general, account balance calculation is not considered\n"
            "                     reliable and has resulted in confusing outcomes, so it is recommended to\n"
            "                     avoid passing this argument.\n"
            "2. minconf           (numeric, optional, default=1) Only include transactions confirmed at least this many times.\n"
            "3. include_watchonly (bool, optional, default=false) Also include balance in watch-only addresses (see 'importaddress')\n"
            "\nResult:\n"
            "amount              (numeric) The total amount in " + CURRENCY_UNIT + " received for this account.\n"
            "\nExamples:\n"
            "\nThe total amount in the wallet with 1 or more confirmations\n"
            + HelpExampleCli("getbalance", "") +
            "\nThe total amount in the wallet at least 6 blocks confirmed\n"
            + HelpExampleCli("getbalance", "\"*\" 6") +
            "\nAs a json rpc call\n"
            + HelpExampleRpc("getbalance", "\"*\", 6")
        );

    ObserveSafeMode();

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pwallet->BlockUntilSyncedToCurrentChain();

    LOCK2(cs_main, pwallet->cs_wallet);

    const UniValue& account_value = request.params[0];
    const UniValue& minconf = request.params[1];
    const UniValue& include_watchonly = request.params[2];

    if (account_value.isNull()) {
        if (!minconf.isNull()) {
            throw JSONRPCError(RPC_INVALID_PARAMETER,
                "getbalance minconf option is only currently supported if an account is specified");
        }
        if (!include_watchonly.isNull()) {
            throw JSONRPCError(RPC_INVALID_PARAMETER,
                "getbalance include_watchonly option is only currently supported if an account is specified");
        }
        return ValueFromAmount(pwallet->GetBalance());
    }

    const std::string& account_param = account_value.get_str();
    const std::string* account = account_param != "*" ? &account_param : nullptr;

    int nMinDepth = 1;
    if (!minconf.isNull())
        nMinDepth = minconf.get_int();
    isminefilter filter = ISMINE_SPENDABLE;
    if(!include_watchonly.isNull())
        if(include_watchonly.get_bool())
            filter = filter | ISMINE_WATCH_ONLY;

    return ValueFromAmount(pwallet->GetLegacyBalance(filter, nMinDepth, account));
}

UniValue getunconfirmedbalance(const JSONRPCRequest &request)
{
    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);
    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || request.params.size() > 0)
        throw std::runtime_error(
                "getunconfirmedbalance\n"
                "Returns the server's total unconfirmed balance\n");

    ObserveSafeMode();

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pwallet->BlockUntilSyncedToCurrentChain();

    LOCK2(cs_main, pwallet->cs_wallet);

    return ValueFromAmount(pwallet->GetUnconfirmedBalance());
}


UniValue movecmd(const JSONRPCRequest& request)
{
    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);
    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || request.params.size() < 3 || request.params.size() > 5)
        throw std::runtime_error(
            "move \"fromaccount\" \"toaccount\" amount ( minconf \"comment\" )\n"
            "\nDEPRECATED. Move a specified amount from one account in your wallet to another.\n"
            "\nArguments:\n"
            "1. \"fromaccount\"   (string, required) The name of the account to move funds from. May be the default account using \"\".\n"
            "2. \"toaccount\"     (string, required) The name of the account to move funds to. May be the default account using \"\".\n"
            "3. amount            (numeric) Quantity of " + CURRENCY_UNIT + " to move between accounts.\n"
            "4. (dummy)           (numeric, optional) Ignored. Remains for backward compatibility.\n"
            "5. \"comment\"       (string, optional) An optional comment, stored in the wallet only.\n"
            "\nResult:\n"
            "true|false           (boolean) true if successful.\n"
            "\nExamples:\n"
            "\nMove 0.01 " + CURRENCY_UNIT + " from the default account to the account named tabby\n"
            + HelpExampleCli("move", "\"\" \"tabby\" 0.01") +
            "\nMove 0.01 " + CURRENCY_UNIT + " timotei to akiko with a comment and funds have 6 confirmations\n"
            + HelpExampleCli("move", "\"timotei\" \"akiko\" 0.01 6 \"happy birthday!\"") +
            "\nAs a json rpc call\n"
            + HelpExampleRpc("move", "\"timotei\", \"akiko\", 0.01, 6, \"happy birthday!\"")
        );

    ObserveSafeMode();
    LOCK2(cs_main, pwallet->cs_wallet);

    std::string strFrom = AccountFromValue(request.params[0]);
    std::string strTo = AccountFromValue(request.params[1]);
    CAmount nAmount = AmountFromValue(request.params[2]);
    if (nAmount <= 0)
        throw JSONRPCError(RPC_TYPE_ERROR, "Invalid amount for send");
    if (!request.params[3].isNull())
        // unused parameter, used to be nMinDepth, keep type-checking it though
        (void)request.params[3].get_int();
    std::string strComment;
    if (!request.params[4].isNull())
        strComment = request.params[4].get_str();

    if (!pwallet->AccountMove(strFrom, strTo, nAmount, strComment)) {
        throw JSONRPCError(RPC_DATABASE_ERROR, "database error");
    }

    return true;
}


UniValue sendfrom(const JSONRPCRequest& request)
{
    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);
    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || request.params.size() < 3 || request.params.size() > 6)
        throw std::runtime_error(
            "sendfrom \"fromaccount\" \"toaddress\" amount ( minconf \"comment\" \"comment_to\" )\n"
            "\nDEPRECATED (use sendtoaddress). Sent an amount from an account to a nix address."
            + HelpRequiringPassphrase(pwallet) + "\n"
            "\nArguments:\n"
            "1. \"fromaccount\"       (string, required) The name of the account to send funds from. May be the default account using \"\".\n"
            "                       Specifying an account does not influence coin selection, but it does associate the newly created\n"
            "                       transaction with the account, so the account's balance computation and transaction history can reflect\n"
            "                       the spend.\n"
            "2. \"toaddress\"         (string, required) The nix address to send funds to.\n"
            "3. amount                (numeric or string, required) The amount in " + CURRENCY_UNIT + " (transaction fee is added on top).\n"
            "4. minconf               (numeric, optional, default=1) Only use funds with at least this many confirmations.\n"
            "5. \"comment\"           (string, optional) A comment used to store what the transaction is for. \n"
            "                                     This is not part of the transaction, just kept in your wallet.\n"
            "6. \"comment_to\"        (string, optional) An optional comment to store the name of the person or organization \n"
            "                                     to which you're sending the transaction. This is not part of the transaction, \n"
            "                                     it is just kept in your wallet.\n"
            "\nResult:\n"
            "\"txid\"                 (string) The transaction id.\n"
            "\nExamples:\n"
            "\nSend 0.01 " + CURRENCY_UNIT + " from the default account to the address, must have at least 1 confirmation\n"
            + HelpExampleCli("sendfrom", "\"\" \"1M72Sfpbz1BPpXFHz9m3CdqATR44Jvaydd\" 0.01") +
            "\nSend 0.01 from the tabby account to the given address, funds must have at least 6 confirmations\n"
            + HelpExampleCli("sendfrom", "\"tabby\" \"1M72Sfpbz1BPpXFHz9m3CdqATR44Jvaydd\" 0.01 6 \"donation\" \"seans outpost\"") +
            "\nAs a json rpc call\n"
            + HelpExampleRpc("sendfrom", "\"tabby\", \"1M72Sfpbz1BPpXFHz9m3CdqATR44Jvaydd\", 0.01, 6, \"donation\", \"seans outpost\"")
        );

    ObserveSafeMode();

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pwallet->BlockUntilSyncedToCurrentChain();

    LOCK2(cs_main, pwallet->cs_wallet);

    std::string strAccount = AccountFromValue(request.params[0]);
    CTxDestination dest = DecodeDestination(request.params[1].get_str());
    if (!IsValidDestination(dest)) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid NIX address");
    }
    CAmount nAmount = AmountFromValue(request.params[2]);
    if (nAmount <= 0)
        throw JSONRPCError(RPC_TYPE_ERROR, "Invalid amount for send");
    int nMinDepth = 1;
    if (!request.params[3].isNull())
        nMinDepth = request.params[3].get_int();

    CWalletTx wtx;
    wtx.strFromAccount = strAccount;
    if (!request.params[4].isNull() && !request.params[4].get_str().empty())
        wtx.mapValue["comment"] = request.params[4].get_str();
    if (!request.params[5].isNull() && !request.params[5].get_str().empty())
        wtx.mapValue["to"]      = request.params[5].get_str();

    EnsureWalletIsUnlocked(pwallet);

    // Check funds
    CAmount nBalance = pwallet->GetLegacyBalance(ISMINE_SPENDABLE, nMinDepth, &strAccount);
    if (nAmount > nBalance)
        throw JSONRPCError(RPC_WALLET_INSUFFICIENT_FUNDS, "Account has insufficient funds");

    CCoinControl no_coin_control; // This is a deprecated API
    SendMoney(pwallet, dest, nAmount, false, wtx, no_coin_control/*, IsStealthAddress(request.params[1].get_str())*/);

    return wtx.GetHash().GetHex();
}


UniValue sendmany(const JSONRPCRequest& request)
{
    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);
    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || request.params.size() < 2 || request.params.size() > 8)
        throw std::runtime_error(
            "sendmany \"fromaccount\" {\"address\":amount,...} ( minconf \"comment\" [\"address\",...] replaceable conf_target \"estimate_mode\")\n"
            "\nSend multiple times. Amounts are double-precision floating point numbers."
            + HelpRequiringPassphrase(pwallet) + "\n"
            "\nArguments:\n"
            "1. \"fromaccount\"         (string, required) DEPRECATED. The account to send the funds from. Should be \"\" for the default account\n"
            "2. \"amounts\"             (string, required) A json object with addresses and amounts\n"
            "    {\n"
            "      \"address\":amount   (numeric or string) The nix address is the key, the numeric amount (can be string) in " + CURRENCY_UNIT + " is the value\n"
            "      ,...\n"
            "    }\n"
            "3. minconf                 (numeric, optional, default=1) Only use the balance confirmed at least this many times.\n"
            "4. \"comment\"             (string, optional) A comment\n"
            "5. subtractfeefrom         (array, optional) A json array with addresses.\n"
            "                           The fee will be equally deducted from the amount of each selected address.\n"
            "                           Those recipients will receive less nix than you enter in their corresponding amount field.\n"
            "                           If no addresses are specified here, the sender pays the fee.\n"
            "    [\n"
            "      \"address\"          (string) Subtract fee from this address\n"
            "      ,...\n"
            "    ]\n"
            "6. replaceable            (boolean, optional) Allow this transaction to be replaced by a transaction with higher fees via BIP 125\n"
            "7. conf_target            (numeric, optional) Confirmation target (in blocks)\n"
            "8. \"estimate_mode\"      (string, optional, default=UNSET) The fee estimate mode, must be one of:\n"
            "       \"UNSET\"\n"
            "       \"ECONOMICAL\"\n"
            "       \"CONSERVATIVE\"\n"
             "\nResult:\n"
            "\"txid\"                   (string) The transaction id for the send. Only 1 transaction is created regardless of \n"
            "                                    the number of addresses.\n"
            "\nExamples:\n"
            "\nSend two amounts to two different addresses:\n"
            + HelpExampleCli("sendmany", "\"\" \"{\\\"1D1ZrZNe3JUo7ZycKEYQQiQAWd9y54F4XX\\\":0.01,\\\"1353tsE8YMTA4EuV7dgUXGjNFf9KpVvKHz\\\":0.02}\"") +
            "\nSend two amounts to two different addresses setting the confirmation and comment:\n"
            + HelpExampleCli("sendmany", "\"\" \"{\\\"1D1ZrZNe3JUo7ZycKEYQQiQAWd9y54F4XX\\\":0.01,\\\"1353tsE8YMTA4EuV7dgUXGjNFf9KpVvKHz\\\":0.02}\" 6 \"testing\"") +
            "\nSend two amounts to two different addresses, subtract fee from amount:\n"
            + HelpExampleCli("sendmany", "\"\" \"{\\\"1D1ZrZNe3JUo7ZycKEYQQiQAWd9y54F4XX\\\":0.01,\\\"1353tsE8YMTA4EuV7dgUXGjNFf9KpVvKHz\\\":0.02}\" 1 \"\" \"[\\\"1D1ZrZNe3JUo7ZycKEYQQiQAWd9y54F4XX\\\",\\\"1353tsE8YMTA4EuV7dgUXGjNFf9KpVvKHz\\\"]\"") +
            "\nAs a json rpc call\n"
            + HelpExampleRpc("sendmany", "\"\", {\"1D1ZrZNe3JUo7ZycKEYQQiQAWd9y54F4XX\":0.01,\"1353tsE8YMTA4EuV7dgUXGjNFf9KpVvKHz\":0.02}, 6, \"testing\"")
        );

    ObserveSafeMode();

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pwallet->BlockUntilSyncedToCurrentChain();

    LOCK2(cs_main, pwallet->cs_wallet);

    if (pwallet->GetBroadcastTransactions() && !g_connman) {
        throw JSONRPCError(RPC_CLIENT_P2P_DISABLED, "Error: Peer-to-peer functionality missing or disabled");
    }

    std::string strAccount = AccountFromValue(request.params[0]);
    UniValue sendTo = request.params[1].get_obj();
    int nMinDepth = 1;
    if (!request.params[2].isNull())
        nMinDepth = request.params[2].get_int();

    CWalletTx wtx;
    wtx.strFromAccount = strAccount;
    if (!request.params[3].isNull() && !request.params[3].get_str().empty())
        wtx.mapValue["comment"] = request.params[3].get_str();

    UniValue subtractFeeFromAmount(UniValue::VARR);
    if (!request.params[4].isNull())
        subtractFeeFromAmount = request.params[4].get_array();

    CCoinControl coin_control;
    if (!request.params[5].isNull()) {
        coin_control.signalRbf = request.params[5].get_bool();
    }

    if (!request.params[6].isNull()) {
        coin_control.m_confirm_target = ParseConfirmTarget(request.params[6]);
    }

    if (!request.params[7].isNull()) {
        if (!FeeModeFromString(request.params[7].get_str(), coin_control.m_fee_mode)) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid estimate_mode parameter");
        }
    }

    std::set<CTxDestination> destinations;
    std::vector<CRecipient> vecSend;

    CAmount totalAmount = 0;
    std::vector<std::string> keys = sendTo.getKeys();
    for (const std::string& name_ : keys) {
        CTxDestination dest = DecodeDestination(name_);
        if (!IsValidDestination(dest)) {
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, std::string("Invalid NIX address: ") + name_);
        }

        if (destinations.count(dest)) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, std::string("Invalid parameter, duplicated address: ") + name_);
        }
        destinations.insert(dest);

        CScript scriptPubKey = GetScriptForDestination(dest);
        CAmount nAmount = AmountFromValue(sendTo[name_]);
        if (nAmount <= 0)
            throw JSONRPCError(RPC_TYPE_ERROR, "Invalid amount for send");
        totalAmount += nAmount;

        bool fSubtractFeeFromAmount = false;
        for (unsigned int idx = 0; idx < subtractFeeFromAmount.size(); idx++) {
            const UniValue& addr = subtractFeeFromAmount[idx];
            if (addr.get_str() == name_)
                fSubtractFeeFromAmount = true;
        }

        CRecipient recipient = {scriptPubKey, nAmount, fSubtractFeeFromAmount};
        vecSend.push_back(recipient);
    }

    EnsureWalletIsUnlocked(pwallet);

    // Check funds
    CAmount nBalance = pwallet->GetLegacyBalance(ISMINE_SPENDABLE, nMinDepth, &strAccount);
    if (totalAmount > nBalance)
        throw JSONRPCError(RPC_WALLET_INSUFFICIENT_FUNDS, "Account has insufficient funds");

    // Send
    CReserveKey keyChange(pwallet);
    CAmount nFeeRequired = 0;
    int nChangePosRet = -1;
    std::string strFailReason;
    bool fCreated = pwallet->CreateTransaction(vecSend, wtx, keyChange, nFeeRequired, nChangePosRet, strFailReason, coin_control);
    if (!fCreated)
        throw JSONRPCError(RPC_WALLET_INSUFFICIENT_FUNDS, strFailReason);
    CValidationState state;
    if (!pwallet->CommitTransaction(wtx, keyChange, g_connman.get(), state)) {
        strFailReason = strprintf("Transaction commit failed:: %s", state.GetRejectReason());
        throw JSONRPCError(RPC_WALLET_ERROR, strFailReason);
    }

    return wtx.GetHash().GetHex();
}

UniValue addmultisigaddress(const JSONRPCRequest& request)
{
    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);
    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || request.params.size() < 2 || request.params.size() > 4) {
        std::string msg = "addmultisigaddress nrequired [\"key\",...] ( \"account\" \"address_type\" )\n"
            "\nAdd a nrequired-to-sign multisignature address to the wallet. Requires a new wallet backup.\n"
            "Each key is a NIX address or hex-encoded public key.\n"
            "This functionality is only intended for use with non-watchonly addresses.\n"
            "See `importaddress` for watchonly p2sh address support.\n"
            "If 'account' is specified (DEPRECATED), assign address to that account.\n"

            "\nArguments:\n"
            "1. nrequired                      (numeric, required) The number of required signatures out of the n keys or addresses.\n"
            "2. \"keys\"                         (string, required) A json array of nix addresses or hex-encoded public keys\n"
            "     [\n"
            "       \"address\"                  (string) nix address or hex-encoded public key\n"
            "       ...,\n"
            "     ]\n"
            "3. \"account\"                      (string, optional) DEPRECATED. An account to assign the addresses to.\n"
            "4. \"address_type\"                 (string, optional) The address type to use. Options are \"legacy\", \"p2sh-segwit\", and \"bech32\". Default is set by -addresstype.\n"

            "\nResult:\n"
            "{\n"
            "  \"address\":\"multisigaddress\",    (string) The value of the new multisig address.\n"
            "  \"redeemScript\":\"script\"         (string) The string value of the hex-encoded redemption script.\n"
            "}\n"
            "\nResult (DEPRECATED. To see this result in v0.16 instead, please start nixd with -deprecatedrpc=addmultisigaddress).\n"
            "        clients should transition to the new output api before upgrading to v0.17.\n"
            "\"address\"                         (string) A nix address associated with the keys.\n"

            "\nExamples:\n"
            "\nAdd a multisig address from 2 addresses\n"
            + HelpExampleCli("addmultisigaddress", "2 \"[\\\"16sSauSf5pF2UkUwvKGq4qjNRzBZYqgEL5\\\",\\\"171sgjn4YtPu27adkKGrdDwzRTxnRkBfKV\\\"]\"") +
            "\nAs json rpc call\n"
            + HelpExampleRpc("addmultisigaddress", "2, \"[\\\"16sSauSf5pF2UkUwvKGq4qjNRzBZYqgEL5\\\",\\\"171sgjn4YtPu27adkKGrdDwzRTxnRkBfKV\\\"]\"")
        ;
        throw std::runtime_error(msg);
    }

    LOCK2(cs_main, pwallet->cs_wallet);

    std::string strAccount;
    if (!request.params[2].isNull())
        strAccount = AccountFromValue(request.params[2]);

    int required = request.params[0].get_int();

    // Get the public keys
    const UniValue& keys_or_addrs = request.params[1].get_array();
    std::vector<CPubKey> pubkeys;
    for (unsigned int i = 0; i < keys_or_addrs.size(); ++i) {
        if (IsHex(keys_or_addrs[i].get_str()) && (keys_or_addrs[i].get_str().length() == 66 || keys_or_addrs[i].get_str().length() == 130)) {
            pubkeys.push_back(HexToPubKey(keys_or_addrs[i].get_str()));
        } else {
            pubkeys.push_back(AddrToPubKey(pwallet, keys_or_addrs[i].get_str()));
        }
    }

    OutputType output_type = g_address_type;
    if (!request.params[3].isNull()) {
        output_type = ParseOutputType(request.params[3].get_str(), output_type);
        if (output_type == OUTPUT_TYPE_NONE) {
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, strprintf("Unknown address type '%s'", request.params[3].get_str()));
        }
    }

    // Construct using pay-to-script-hash:
    CScript inner = CreateMultisigRedeemscript(required, pubkeys);
    pwallet->AddCScript(inner);
    CTxDestination dest = pwallet->AddAndGetDestinationForScript(inner, output_type);
    pwallet->SetAddressBook(dest, strAccount, "send");

    // Return old style interface
    if (IsDeprecatedRPCEnabled("addmultisigaddress")) {
        return EncodeDestination(dest);
    }

    UniValue result(UniValue::VOBJ);
    result.pushKV("address", EncodeDestination(dest));
    result.pushKV("redeemScript", HexStr(inner.begin(), inner.end()));
    return result;
}

class Witnessifier : public boost::static_visitor<bool>
{
public:
    CWallet * const pwallet;
    CTxDestination result;
    bool already_witness;

    explicit Witnessifier(CWallet *_pwallet) : pwallet(_pwallet), already_witness(false) {}

    bool operator()(const CKeyID &keyID) {
        if (pwallet) {
            CScript basescript = GetScriptForDestination(keyID);
            CScript witscript = GetScriptForWitness(basescript);
            if (!IsSolvable(*pwallet, witscript, false)) {
                return false;
            }
            return ExtractDestination(witscript, result);
        }
        return false;
    }

    bool operator()(const CScriptID &scriptID) {
        CScript subscript;
        if (pwallet && pwallet->GetCScript(scriptID, subscript)) {
            int witnessversion;
            std::vector<unsigned char> witprog;
            if (subscript.IsWitnessProgram(witnessversion, witprog, false)) {
                ExtractDestination(subscript, result);
                already_witness = true;
                return true;
            }
            CScript witscript = GetScriptForWitness(subscript);
            if (!IsSolvable(*pwallet, witscript, false)) {
                return false;
            }
            return ExtractDestination(witscript, result);
        }
        return false;
    }

    bool operator()(const WitnessV0KeyHash& id)
    {
        already_witness = true;
        result = id;
        return true;
    }

    bool operator()(const WitnessV0ScriptHash& id)
    {
        already_witness = true;
        result = id;
        return true;
    }

    template<typename T>
    bool operator()(const T& dest) { return false; }
};

UniValue addwitnessaddress(const JSONRPCRequest& request)
{
    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);
    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || request.params.size() < 1 || request.params.size() > 2)
    {
        std::string msg = "addwitnessaddress \"address\" ( p2sh )\n"
            "\nDEPRECATED: set the address_type argument of getnewaddress, or option -addresstype=[bech32|p2sh-segwit] instead.\n"
            "Add a witness address for a script (with pubkey or redeemscript known). Requires a new wallet backup.\n"
            "It returns the witness script.\n"

            "\nArguments:\n"
            "1. \"address\"       (string, required) An address known to the wallet\n"
            "2. p2sh            (bool, optional, default=true) Embed inside P2SH\n"

            "\nResult:\n"
            "\"witnessaddress\",  (string) The value of the new address (P2SH or BIP173).\n"
            "}\n"
        ;
        throw std::runtime_error(msg);
    }

    if (!IsDeprecatedRPCEnabled("addwitnessaddress")) {
        throw JSONRPCError(RPC_METHOD_DEPRECATED, "addwitnessaddress is deprecated and will be fully removed in v0.17. "
            "To use addwitnessaddress in v0.16, restart nixd with -deprecatedrpc=addwitnessaddress.\n"
            "Projects should transition to using the address_type argument of getnewaddress, or option -addresstype=[bech32|p2sh-segwit] instead.\n");
    }

    {
        LOCK(cs_main);
        if (!IsWitnessEnabled(chainActive.Tip(), Params().GetConsensus()) && !gArgs.GetBoolArg("-walletprematurewitness", false)) {
            throw JSONRPCError(RPC_WALLET_ERROR, "Segregated witness not enabled on network");
        }
    }

    CTxDestination dest = DecodeDestination(request.params[0].get_str());
    if (!IsValidDestination(dest)) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid NIX address");
    }

    bool p2sh = true;
    if (!request.params[1].isNull()) {
        p2sh = request.params[1].get_bool();
    }

    Witnessifier w(pwallet);
    bool ret = boost::apply_visitor(w, dest);
    if (!ret) {
        throw JSONRPCError(RPC_WALLET_ERROR, "Public key or redeemscript not known to wallet, or the key is uncompressed");
    }

    CScript witprogram = GetScriptForDestination(w.result);

    if (p2sh) {
        w.result = CScriptID(witprogram);
    }

    if (w.already_witness) {
        if (!(dest == w.result)) {
            throw JSONRPCError(RPC_WALLET_ERROR, "Cannot convert between witness address types");
        }
    } else {
        pwallet->AddCScript(witprogram); // Implicit for single-key now, but necessary for multisig and for compatibility with older software
        pwallet->SetAddressBook(w.result, "", "receive");
    }

    return EncodeDestination(w.result);
}

struct tallyitem
{
    CAmount nAmount;
    int nConf;
    std::vector<uint256> txids;
    bool fIsWatchonly;
    tallyitem()
    {
        nAmount = 0;
        nConf = std::numeric_limits<int>::max();
        fIsWatchonly = false;
    }
};

UniValue ListReceived(CWallet * const pwallet, const UniValue& params, bool fByAccounts)
{
    // Minimum confirmations
    int nMinDepth = 1;
    if (!params[0].isNull())
        nMinDepth = params[0].get_int();

    // Whether to include empty accounts
    bool fIncludeEmpty = false;
    if (!params[1].isNull())
        fIncludeEmpty = params[1].get_bool();

    isminefilter filter = ISMINE_SPENDABLE;
    if(!params[2].isNull())
        if(params[2].get_bool())
            filter = filter | ISMINE_WATCH_ONLY;

    // Tally
    std::map<CTxDestination, tallyitem> mapTally;
    for (const std::pair<uint256, CWalletTx>& pairWtx : pwallet->mapWallet) {
        const CWalletTx& wtx = pairWtx.second;

        if (wtx.IsCoinBase() || !CheckFinalTx(*wtx.tx))
            continue;

        int nDepth = wtx.GetDepthInMainChain();
        if (nDepth < nMinDepth)
            continue;

        for (const CTxOut& txout : wtx.tx->vout)
        {
            CTxDestination address;
            if (!ExtractDestination(txout.scriptPubKey, address))
                continue;

            isminefilter mine = IsMine(*pwallet, address);
            if(!(mine & filter))
                continue;

            tallyitem& item = mapTally[address];
            item.nAmount += txout.nValue;
            item.nConf = std::min(item.nConf, nDepth);
            item.txids.push_back(wtx.GetHash());
            if (mine & ISMINE_WATCH_ONLY)
                item.fIsWatchonly = true;
        }
    }

    // Reply
    UniValue ret(UniValue::VARR);
    std::map<std::string, tallyitem> mapAccountTally;
    for (const std::pair<CTxDestination, CAddressBookData>& item : pwallet->mapAddressBook) {
        const CTxDestination& dest = item.first;
        const std::string& strAccount = item.second.name;
        std::map<CTxDestination, tallyitem>::iterator it = mapTally.find(dest);
        if (it == mapTally.end() && !fIncludeEmpty)
            continue;

        CAmount nAmount = 0;
        int nConf = std::numeric_limits<int>::max();
        bool fIsWatchonly = false;
        if (it != mapTally.end())
        {
            nAmount = (*it).second.nAmount;
            nConf = (*it).second.nConf;
            fIsWatchonly = (*it).second.fIsWatchonly;
        }

        if (fByAccounts)
        {
            tallyitem& _item = mapAccountTally[strAccount];
            _item.nAmount += nAmount;
            _item.nConf = std::min(_item.nConf, nConf);
            _item.fIsWatchonly = fIsWatchonly;
        }
        else
        {
            UniValue obj(UniValue::VOBJ);
            if(fIsWatchonly)
                obj.push_back(Pair("involvesWatchonly", true));
            obj.push_back(Pair("address",       EncodeDestination(dest)));
            obj.push_back(Pair("account",       strAccount));
            obj.push_back(Pair("amount",        ValueFromAmount(nAmount)));
            obj.push_back(Pair("confirmations", (nConf == std::numeric_limits<int>::max() ? 0 : nConf)));
            if (!fByAccounts)
                obj.push_back(Pair("label", strAccount));
            UniValue transactions(UniValue::VARR);
            if (it != mapTally.end())
            {
                for (const uint256& _item : (*it).second.txids)
                {
                    transactions.push_back(_item.GetHex());
                }
            }
            obj.push_back(Pair("txids", transactions));
            ret.push_back(obj);
        }
    }

    if (fByAccounts)
    {
        for (const auto& entry : mapAccountTally)
        {
            CAmount nAmount = entry.second.nAmount;
            int nConf = entry.second.nConf;
            UniValue obj(UniValue::VOBJ);
            if (entry.second.fIsWatchonly)
                obj.push_back(Pair("involvesWatchonly", true));
            obj.push_back(Pair("account",       entry.first));
            obj.push_back(Pair("amount",        ValueFromAmount(nAmount)));
            obj.push_back(Pair("confirmations", (nConf == std::numeric_limits<int>::max() ? 0 : nConf)));
            ret.push_back(obj);
        }
    }

    return ret;
}

UniValue listreceivedbyaddress(const JSONRPCRequest& request)
{
    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);
    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || request.params.size() > 3)
        throw std::runtime_error(
            "listreceivedbyaddress ( minconf include_empty include_watchonly)\n"
            "\nList balances by receiving address.\n"
            "\nArguments:\n"
            "1. minconf           (numeric, optional, default=1) The minimum number of confirmations before payments are included.\n"
            "2. include_empty     (bool, optional, default=false) Whether to include addresses that haven't received any payments.\n"
            "3. include_watchonly (bool, optional, default=false) Whether to include watch-only addresses (see 'importaddress').\n"

            "\nResult:\n"
            "[\n"
            "  {\n"
            "    \"involvesWatchonly\" : true,        (bool) Only returned if imported addresses were involved in transaction\n"
            "    \"address\" : \"receivingaddress\",  (string) The receiving address\n"
            "    \"account\" : \"accountname\",       (string) DEPRECATED. The account of the receiving address. The default account is \"\".\n"
            "    \"amount\" : x.xxx,                  (numeric) The total amount in " + CURRENCY_UNIT + " received by the address\n"
            "    \"confirmations\" : n,               (numeric) The number of confirmations of the most recent transaction included\n"
            "    \"label\" : \"label\",               (string) A comment for the address/transaction, if any\n"
            "    \"txids\": [\n"
            "       n,                                (numeric) The ids of transactions received with the address \n"
            "       ...\n"
            "    ]\n"
            "  }\n"
            "  ,...\n"
            "]\n"

            "\nExamples:\n"
            + HelpExampleCli("listreceivedbyaddress", "")
            + HelpExampleCli("listreceivedbyaddress", "6 true")
            + HelpExampleRpc("listreceivedbyaddress", "6, true, true")
        );

    ObserveSafeMode();

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pwallet->BlockUntilSyncedToCurrentChain();

    LOCK2(cs_main, pwallet->cs_wallet);

    return ListReceived(pwallet, request.params, false);
}

UniValue listreceivedbyaccount(const JSONRPCRequest& request)
{
    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);
    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || request.params.size() > 3)
        throw std::runtime_error(
            "listreceivedbyaccount ( minconf include_empty include_watchonly)\n"
            "\nDEPRECATED. List balances by account.\n"
            "\nArguments:\n"
            "1. minconf           (numeric, optional, default=1) The minimum number of confirmations before payments are included.\n"
            "2. include_empty     (bool, optional, default=false) Whether to include accounts that haven't received any payments.\n"
            "3. include_watchonly (bool, optional, default=false) Whether to include watch-only addresses (see 'importaddress').\n"

            "\nResult:\n"
            "[\n"
            "  {\n"
            "    \"involvesWatchonly\" : true,   (bool) Only returned if imported addresses were involved in transaction\n"
            "    \"account\" : \"accountname\",  (string) The account name of the receiving account\n"
            "    \"amount\" : x.xxx,             (numeric) The total amount received by addresses with this account\n"
            "    \"confirmations\" : n,          (numeric) The number of confirmations of the most recent transaction included\n"
            "    \"label\" : \"label\"           (string) A comment for the address/transaction, if any\n"
            "  }\n"
            "  ,...\n"
            "]\n"

            "\nExamples:\n"
            + HelpExampleCli("listreceivedbyaccount", "")
            + HelpExampleCli("listreceivedbyaccount", "6 true")
            + HelpExampleRpc("listreceivedbyaccount", "6, true, true")
        );

    ObserveSafeMode();

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pwallet->BlockUntilSyncedToCurrentChain();

    LOCK2(cs_main, pwallet->cs_wallet);

    return ListReceived(pwallet, request.params, true);
}

static void MaybePushAddress(UniValue & entry, const CTxDestination &dest)
{
    if (IsValidDestination(dest)) {
        entry.push_back(Pair("address", EncodeDestination(dest)));
    }
}

/**
 * List transactions based on the given criteria.
 *
 * @param  pwallet    The wallet.
 * @param  wtx        The wallet transaction.
 * @param  strAccount The account, if any, or "*" for all.
 * @param  nMinDepth  The minimum confirmation depth.
 * @param  fLong      Whether to include the JSON version of the transaction.
 * @param  ret        The UniValue into which the result is stored.
 * @param  filter     The "is mine" filter bool.
 */
void ListTransactions(CWallet* const pwallet, const CWalletTx& wtx, const std::string& strAccount, int nMinDepth, bool fLong, UniValue& ret, const isminefilter& filter)
{
    CAmount nFee;
    std::string strSentAccount;
    std::list<COutputEntry> listReceived;
    std::list<COutputEntry> listSent;
    std::list<COutputEntry> listStaked;


    wtx.GetAmounts(listReceived, listSent, listStaked, nFee, strSentAccount, filter);

    bool fAllAccounts = (strAccount == std::string("*"));
    bool involvesWatchonly = wtx.IsFromMe(ISMINE_WATCH_ONLY);

    // Sent
    if ((!listSent.empty() || nFee != 0) && (fAllAccounts || strAccount == strSentAccount))
    {
        for (const COutputEntry& s : listSent)
        {
            UniValue entry(UniValue::VOBJ);
            if (involvesWatchonly || (::IsMine(*pwallet, s.destination) & ISMINE_WATCH_ONLY)) {
                entry.push_back(Pair("involvesWatchonly", true));
            }
            entry.push_back(Pair("account", strSentAccount));
            MaybePushAddress(entry, s.destination);
            entry.push_back(Pair("category", "send"));
            entry.push_back(Pair("amount", ValueFromAmount(-s.amount)));
            if (pwallet->mapAddressBook.count(s.destination)) {
                entry.push_back(Pair("label", pwallet->mapAddressBook[s.destination].name));
            }
            entry.push_back(Pair("vout", s.vout));
            entry.push_back(Pair("fee", ValueFromAmount(-nFee)));
            if (fLong)
                WalletTxToJSON(wtx, entry);
            entry.push_back(Pair("abandoned", wtx.isAbandoned()));
            entry.push_back(Pair("is_ghosted", wtx.tx->IsZerocoinMint() || wtx.tx->IsZerocoinSpend()));
            ret.push_back(entry);
        }
    }

    // Received
    if (listReceived.size() > 0 && wtx.GetDepthInMainChain() >= nMinDepth)
    {
        for (const COutputEntry& r : listReceived)
        {
            std::string account;
            if (pwallet->mapAddressBook.count(r.destination)) {
                account = pwallet->mapAddressBook[r.destination].name;
            }
            if (fAllAccounts || (account == strAccount))
            {
                UniValue entry(UniValue::VOBJ);
                if (involvesWatchonly || (::IsMine(*pwallet, r.destination) & ISMINE_WATCH_ONLY)) {
                    entry.push_back(Pair("involvesWatchonly", true));
                }
                entry.push_back(Pair("account", account));
                MaybePushAddress(entry, r.destination);
                if (wtx.IsCoinBase())
                {
                    if (wtx.GetDepthInMainChain() < 1)
                        entry.push_back(Pair("category", "orphan"));
                    else if (wtx.GetBlocksToMaturity() > 0)
                        entry.push_back(Pair("category", "immature"));
                    else
                        entry.push_back(Pair("category", "generate"));
                }
                else
                {
                    entry.push_back(Pair("category", "receive"));
                }
                entry.push_back(Pair("amount", ValueFromAmount(r.amount)));
                if (pwallet->mapAddressBook.count(r.destination)) {
                    entry.push_back(Pair("label", account));
                }
                entry.push_back(Pair("vout", r.vout));
                if (fLong)
                    WalletTxToJSON(wtx, entry);
                entry.push_back(Pair("is_unghosted", wtx.tx->IsZerocoinSpend()));
                ret.push_back(entry);
            }
        }
    }

    // Staked
    if (listStaked.size() > 0 && wtx.GetDepthInMainChain() >= nMinDepth)
    {
        for (const auto &s : listStaked)
        {
            UniValue entry(UniValue::VOBJ);
            if (involvesWatchonly || (s.ismine & ISMINE_WATCH_ONLY))
                entry.push_back(Pair("involvesWatchonly", true));
            //entry.push_back(Pair("account", strSentAccount));
            MaybePushAddress(entry, s.destination);
            if (s.destStake.type() != typeid(CNoDestination))
                entry.push_back(Pair("coldstake_address", EncodeDestination(s.destStake)));

            if (wtx.GetDepthInMainChain() < 1)
                entry.push_back(Pair("category", "orphaned_stake"));
            else
                entry.push_back(Pair("category", "stake"));

            entry.push_back(Pair("amount", ValueFromAmount(s.amount)));
            if (pwallet->mapAddressBook.count(s.destination))
                entry.push_back(Pair("label", pwallet->mapAddressBook[s.destination].name));
            entry.push_back(Pair("vout", s.vout));
            entry.push_back(Pair("reward", ValueFromAmount(-nFee)));
            if (fLong)
                WalletTxToJSON(wtx, entry);
            entry.push_back(Pair("abandoned", wtx.isAbandoned()));
            ret.push_back(entry);
        }
    }
}

void AcentryToJSON(const CAccountingEntry& acentry, const std::string& strAccount, UniValue& ret)
{
    bool fAllAccounts = (strAccount == std::string("*"));

    if (fAllAccounts || acentry.strAccount == strAccount)
    {
        UniValue entry(UniValue::VOBJ);
        entry.push_back(Pair("account", acentry.strAccount));
        entry.push_back(Pair("category", "move"));
        entry.push_back(Pair("time", acentry.nTime));
        entry.push_back(Pair("amount", ValueFromAmount(acentry.nCreditDebit)));
        entry.push_back(Pair("otheraccount", acentry.strOtherAccount));
        entry.push_back(Pair("comment", acentry.strComment));
        ret.push_back(entry);
    }
}

UniValue listtransactions(const JSONRPCRequest& request)
{
    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);
    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || request.params.size() > 4)
        throw std::runtime_error(
            "listtransactions ( \"account\" count skip include_watchonly)\n"
            "\nReturns up to 'count' most recent transactions skipping the first 'from' transactions for account 'account'.\n"
            "\nArguments:\n"
            "1. \"account\"    (string, optional) DEPRECATED. The account name. Should be \"*\".\n"
            "2. count          (numeric, optional, default=10) The number of transactions to return\n"
            "3. skip           (numeric, optional, default=0) The number of transactions to skip\n"
            "4. include_watchonly (bool, optional, default=false) Include transactions to watch-only addresses (see 'importaddress')\n"
            "\nResult:\n"
            "[\n"
            "  {\n"
            "    \"account\":\"accountname\",       (string) DEPRECATED. The account name associated with the transaction. \n"
            "                                                It will be \"\" for the default account.\n"
            "    \"address\":\"address\",    (string) The nix address of the transaction. Not present for \n"
            "                                                move transactions (category = move).\n"
            "    \"category\":\"send|receive|move\", (string) The transaction category. 'move' is a local (off blockchain)\n"
            "                                                transaction between accounts, and not associated with an address,\n"
            "                                                transaction id or block. 'send' and 'receive' transactions are \n"
            "                                                associated with an address, transaction id and block details\n"
            "    \"amount\": x.xxx,          (numeric) The amount in " + CURRENCY_UNIT + ". This is negative for the 'send' category, and for the\n"
            "                                         'move' category for moves outbound. It is positive for the 'receive' category,\n"
            "                                         and for the 'move' category for inbound funds.\n"
            "    \"label\": \"label\",       (string) A comment for the address/transaction, if any\n"
            "    \"vout\": n,                (numeric) the vout value\n"
            "    \"fee\": x.xxx,             (numeric) The amount of the fee in " + CURRENCY_UNIT + ". This is negative and only available for the \n"
            "                                         'send' category of transactions.\n"
            "    \"confirmations\": n,       (numeric) The number of confirmations for the transaction. Available for 'send' and \n"
            "                                         'receive' category of transactions. Negative confirmations indicate the\n"
            "                                         transaction conflicts with the block chain\n"
            "    \"trusted\": xxx,           (bool) Whether we consider the outputs of this unconfirmed transaction safe to spend.\n"
            "    \"blockhash\": \"hashvalue\", (string) The block hash containing the transaction. Available for 'send' and 'receive'\n"
            "                                          category of transactions.\n"
            "    \"blockindex\": n,          (numeric) The index of the transaction in the block that includes it. Available for 'send' and 'receive'\n"
            "                                          category of transactions.\n"
            "    \"blocktime\": xxx,         (numeric) The block time in seconds since epoch (1 Jan 1970 GMT).\n"
            "    \"txid\": \"transactionid\", (string) The transaction id. Available for 'send' and 'receive' category of transactions.\n"
            "    \"time\": xxx,              (numeric) The transaction time in seconds since epoch (midnight Jan 1 1970 GMT).\n"
            "    \"timereceived\": xxx,      (numeric) The time received in seconds since epoch (midnight Jan 1 1970 GMT). Available \n"
            "                                          for 'send' and 'receive' category of transactions.\n"
            "    \"comment\": \"...\",       (string) If a comment is associated with the transaction.\n"
            "    \"otheraccount\": \"accountname\",  (string) DEPRECATED. For the 'move' category of transactions, the account the funds came \n"
            "                                          from (for receiving funds, positive amounts), or went to (for sending funds,\n"
            "                                          negative amounts).\n"
            "    \"bip125-replaceable\": \"yes|no|unknown\",  (string) Whether this transaction could be replaced due to BIP125 (replace-by-fee);\n"
            "                                                     may be unknown for unconfirmed transactions not in the mempool\n"
            "    \"abandoned\": xxx          (bool) 'true' if the transaction has been abandoned (inputs are respendable). Only available for the \n"
            "                                         'send' category of transactions.\n"
            "  }\n"
            "]\n"

            "\nExamples:\n"
            "\nList the most recent 10 transactions in the systems\n"
            + HelpExampleCli("listtransactions", "") +
            "\nList transactions 100 to 120\n"
            + HelpExampleCli("listtransactions", "\"*\" 20 100") +
            "\nAs a json rpc call\n"
            + HelpExampleRpc("listtransactions", "\"*\", 20, 100")
        );

    ObserveSafeMode();

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pwallet->BlockUntilSyncedToCurrentChain();

    LOCK2(cs_main, pwallet->cs_wallet);

    std::string strAccount = "*";
    if (!request.params[0].isNull())
        strAccount = request.params[0].get_str();
    int nCount = 10;
    if (!request.params[1].isNull())
        nCount = request.params[1].get_int();
    int nFrom = 0;
    if (!request.params[2].isNull())
        nFrom = request.params[2].get_int();
    isminefilter filter = ISMINE_SPENDABLE | ISMINE_WATCH_COLDSTAKE;
    if(!request.params[3].isNull())
        if(request.params[3].get_bool())
            filter = filter | ISMINE_WATCH_ONLY;

    if (nCount < 0)
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Negative count");
    if (nFrom < 0)
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Negative from");

    UniValue ret(UniValue::VARR);

    const CWallet::TxItems & txOrdered = pwallet->wtxOrdered;

    // iterate backwards until we have nCount items to return:
    for (CWallet::TxItems::const_reverse_iterator it = txOrdered.rbegin(); it != txOrdered.rend(); ++it)
    {
        CWalletTx *const pwtx = (*it).second.first;
        if (pwtx != nullptr)
            ListTransactions(pwallet, *pwtx, strAccount, 0, true, ret, filter);
        CAccountingEntry *const pacentry = (*it).second.second;
        if (pacentry != nullptr)
            AcentryToJSON(*pacentry, strAccount, ret);

        if ((int)ret.size() >= (nCount+nFrom)) break;
    }
    // ret is newest to oldest

    if (nFrom > (int)ret.size())
        nFrom = ret.size();
    if ((nFrom + nCount) > (int)ret.size())
        nCount = ret.size() - nFrom;

    std::vector<UniValue> arrTmp = ret.getValues();

    std::vector<UniValue>::iterator first = arrTmp.begin();
    std::advance(first, nFrom);
    std::vector<UniValue>::iterator last = arrTmp.begin();
    std::advance(last, nFrom+nCount);

    if (last != arrTmp.end()) arrTmp.erase(last, arrTmp.end());
    if (first != arrTmp.begin()) arrTmp.erase(arrTmp.begin(), first);

    std::reverse(arrTmp.begin(), arrTmp.end()); // Return oldest to newest

    ret.clear();
    ret.setArray();
    ret.push_backV(arrTmp);

    return ret;
}

UniValue listaccounts(const JSONRPCRequest& request)
{
    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);
    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || request.params.size() > 2)
        throw std::runtime_error(
            "listaccounts ( minconf include_watchonly)\n"
            "\nDEPRECATED. Returns Object that has account names as keys, account balances as values.\n"
            "\nArguments:\n"
            "1. minconf             (numeric, optional, default=1) Only include transactions with at least this many confirmations\n"
            "2. include_watchonly   (bool, optional, default=false) Include balances in watch-only addresses (see 'importaddress')\n"
            "\nResult:\n"
            "{                      (json object where keys are account names, and values are numeric balances\n"
            "  \"account\": x.xxx,  (numeric) The property name is the account name, and the value is the total balance for the account.\n"
            "  ...\n"
            "}\n"
            "\nExamples:\n"
            "\nList account balances where there at least 1 confirmation\n"
            + HelpExampleCli("listaccounts", "") +
            "\nList account balances including zero confirmation transactions\n"
            + HelpExampleCli("listaccounts", "0") +
            "\nList account balances for 6 or more confirmations\n"
            + HelpExampleCli("listaccounts", "6") +
            "\nAs json rpc call\n"
            + HelpExampleRpc("listaccounts", "6")
        );

    ObserveSafeMode();

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pwallet->BlockUntilSyncedToCurrentChain();

    LOCK2(cs_main, pwallet->cs_wallet);

    int nMinDepth = 1;
    if (!request.params[0].isNull())
        nMinDepth = request.params[0].get_int();
    isminefilter includeWatchonly = ISMINE_SPENDABLE;
    if(!request.params[1].isNull())
        if(request.params[1].get_bool())
            includeWatchonly = includeWatchonly | ISMINE_WATCH_ONLY;

    std::map<std::string, CAmount> mapAccountBalances;
    for (const std::pair<CTxDestination, CAddressBookData>& entry : pwallet->mapAddressBook) {
        if (IsMine(*pwallet, entry.first) & includeWatchonly) {  // This address belongs to me
            mapAccountBalances[entry.second.name] = 0;
        }
    }

    for (const std::pair<uint256, CWalletTx>& pairWtx : pwallet->mapWallet) {
        const CWalletTx& wtx = pairWtx.second;
        CAmount nFee;
        std::string strSentAccount;
        std::list<COutputEntry> listReceived;
        std::list<COutputEntry> listSent;
        std::list<COutputEntry> listStaked;
        int nDepth = wtx.GetDepthInMainChain();
        if (wtx.GetBlocksToMaturity() > 0 || nDepth < 0)
            continue;
        wtx.GetAmounts(listReceived, listSent, listStaked, nFee, strSentAccount, includeWatchonly);
        mapAccountBalances[strSentAccount] -= nFee;
        for (const COutputEntry& s : listSent)
            mapAccountBalances[strSentAccount] -= s.amount;
        if (nDepth >= nMinDepth)
        {
            for (const COutputEntry& r : listReceived)
                if (pwallet->mapAddressBook.count(r.destination)) {
                    mapAccountBalances[pwallet->mapAddressBook[r.destination].name] += r.amount;
                }
                else
                    mapAccountBalances[""] += r.amount;
        }
    }

    const std::list<CAccountingEntry>& acentries = pwallet->laccentries;
    for (const CAccountingEntry& entry : acentries)
        mapAccountBalances[entry.strAccount] += entry.nCreditDebit;

    UniValue ret(UniValue::VOBJ);
    for (const std::pair<std::string, CAmount>& accountBalance : mapAccountBalances) {
        ret.push_back(Pair(accountBalance.first, ValueFromAmount(accountBalance.second)));
    }
    return ret;
}

UniValue listsinceblock(const JSONRPCRequest& request)
{
    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);
    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || request.params.size() > 4)
        throw std::runtime_error(
            "listsinceblock ( \"blockhash\" target_confirmations include_watchonly include_removed )\n"
            "\nGet all transactions in blocks since block [blockhash], or all transactions if omitted.\n"
            "If \"blockhash\" is no longer a part of the main chain, transactions from the fork point onward are included.\n"
            "Additionally, if include_removed is set, transactions affecting the wallet which were removed are returned in the \"removed\" array.\n"
            "\nArguments:\n"
            "1. \"blockhash\"            (string, optional) The block hash to list transactions since\n"
            "2. target_confirmations:    (numeric, optional, default=1) Return the nth block hash from the main chain. e.g. 1 would mean the best block hash. Note: this is not used as a filter, but only affects [lastblock] in the return value\n"
            "3. include_watchonly:       (bool, optional, default=false) Include transactions to watch-only addresses (see 'importaddress')\n"
            "4. include_removed:         (bool, optional, default=true) Show transactions that were removed due to a reorg in the \"removed\" array\n"
            "                                                           (not guaranteed to work on pruned nodes)\n"
            "\nResult:\n"
            "{\n"
            "  \"transactions\": [\n"
            "    \"account\":\"accountname\",       (string) DEPRECATED. The account name associated with the transaction. Will be \"\" for the default account.\n"
            "    \"address\":\"address\",    (string) The nix address of the transaction. Not present for move transactions (category = move).\n"
            "    \"category\":\"send|receive\",     (string) The transaction category. 'send' has negative amounts, 'receive' has positive amounts.\n"
            "    \"amount\": x.xxx,          (numeric) The amount in " + CURRENCY_UNIT + ". This is negative for the 'send' category, and for the 'move' category for moves \n"
            "                                          outbound. It is positive for the 'receive' category, and for the 'move' category for inbound funds.\n"
            "    \"vout\" : n,               (numeric) the vout value\n"
            "    \"fee\": x.xxx,             (numeric) The amount of the fee in " + CURRENCY_UNIT + ". This is negative and only available for the 'send' category of transactions.\n"
            "    \"confirmations\": n,       (numeric) The number of confirmations for the transaction. Available for 'send' and 'receive' category of transactions.\n"
            "                                          When it's < 0, it means the transaction conflicted that many blocks ago.\n"
            "    \"blockhash\": \"hashvalue\",     (string) The block hash containing the transaction. Available for 'send' and 'receive' category of transactions.\n"
            "    \"blockindex\": n,          (numeric) The index of the transaction in the block that includes it. Available for 'send' and 'receive' category of transactions.\n"
            "    \"blocktime\": xxx,         (numeric) The block time in seconds since epoch (1 Jan 1970 GMT).\n"
            "    \"txid\": \"transactionid\",  (string) The transaction id. Available for 'send' and 'receive' category of transactions.\n"
            "    \"time\": xxx,              (numeric) The transaction time in seconds since epoch (Jan 1 1970 GMT).\n"
            "    \"timereceived\": xxx,      (numeric) The time received in seconds since epoch (Jan 1 1970 GMT). Available for 'send' and 'receive' category of transactions.\n"
            "    \"bip125-replaceable\": \"yes|no|unknown\",  (string) Whether this transaction could be replaced due to BIP125 (replace-by-fee);\n"
            "                                                   may be unknown for unconfirmed transactions not in the mempool\n"
            "    \"abandoned\": xxx,         (bool) 'true' if the transaction has been abandoned (inputs are respendable). Only available for the 'send' category of transactions.\n"
            "    \"comment\": \"...\",       (string) If a comment is associated with the transaction.\n"
            "    \"label\" : \"label\"       (string) A comment for the address/transaction, if any\n"
            "    \"to\": \"...\",            (string) If a comment to is associated with the transaction.\n"
            "  ],\n"
            "  \"removed\": [\n"
            "    <structure is the same as \"transactions\" above, only present if include_removed=true>\n"
            "    Note: transactions that were readded in the active chain will appear as-is in this array, and may thus have a positive confirmation count.\n"
            "  ],\n"
            "  \"lastblock\": \"lastblockhash\"     (string) The hash of the block (target_confirmations-1) from the best block on the main chain. This is typically used to feed back into listsinceblock the next time you call it. So you would generally use a target_confirmations of say 6, so you will be continually re-notified of transactions until they've reached 6 confirmations plus any new ones\n"
            "}\n"
            "\nExamples:\n"
            + HelpExampleCli("listsinceblock", "")
            + HelpExampleCli("listsinceblock", "\"000000000000000bacf66f7497b7dc45ef753ee9a7d38571037cdb1a57f663ad\" 6")
            + HelpExampleRpc("listsinceblock", "\"000000000000000bacf66f7497b7dc45ef753ee9a7d38571037cdb1a57f663ad\", 6")
        );

    ObserveSafeMode();

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pwallet->BlockUntilSyncedToCurrentChain();

    LOCK2(cs_main, pwallet->cs_wallet);

    const CBlockIndex* pindex = nullptr;    // Block index of the specified block or the common ancestor, if the block provided was in a deactivated chain.
    const CBlockIndex* paltindex = nullptr; // Block index of the specified block, even if it's in a deactivated chain.
    int target_confirms = 1;
    isminefilter filter = ISMINE_SPENDABLE;

    if (!request.params[0].isNull() && !request.params[0].get_str().empty()) {
        uint256 blockId;

        blockId.SetHex(request.params[0].get_str());
        BlockMap::iterator it = mapBlockIndex.find(blockId);
        if (it == mapBlockIndex.end()) {
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Block not found");
        }
        paltindex = pindex = it->second;
        if (chainActive[pindex->nHeight] != pindex) {
            // the block being asked for is a part of a deactivated chain;
            // we don't want to depend on its perceived height in the block
            // chain, we want to instead use the last common ancestor
            pindex = chainActive.FindFork(pindex);
        }
    }

    if (!request.params[1].isNull()) {
        target_confirms = request.params[1].get_int();

        if (target_confirms < 1) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter");
        }
    }

    if (!request.params[2].isNull() && request.params[2].get_bool()) {
        filter = filter | ISMINE_WATCH_ONLY;
    }

    bool include_removed = (request.params[3].isNull() || request.params[3].get_bool());

    int depth = pindex ? (1 + chainActive.Height() - pindex->nHeight) : -1;

    UniValue transactions(UniValue::VARR);

    for (const std::pair<uint256, CWalletTx>& pairWtx : pwallet->mapWallet) {
        CWalletTx tx = pairWtx.second;

        if (depth == -1 || tx.GetDepthInMainChain() < depth) {
            ListTransactions(pwallet, tx, "*", 0, true, transactions, filter);
        }
    }

    // when a reorg'd block is requested, we also list any relevant transactions
    // in the blocks of the chain that was detached
    UniValue removed(UniValue::VARR);
    while (include_removed && paltindex && paltindex != pindex) {
        CBlock block;
        if (!ReadBlockFromDisk(block, paltindex, Params().GetConsensus())) {
            throw JSONRPCError(RPC_INTERNAL_ERROR, "Can't read block from disk");
        }
        for (const CTransactionRef& tx : block.vtx) {
            auto it = pwallet->mapWallet.find(tx->GetHash());
            if (it != pwallet->mapWallet.end()) {
                // We want all transactions regardless of confirmation count to appear here,
                // even negative confirmation ones, hence the big negative.
                ListTransactions(pwallet, it->second, "*", -100000000, true, removed, filter);
            }
        }
        paltindex = paltindex->pprev;
    }

    CBlockIndex *pblockLast = chainActive[chainActive.Height() + 1 - target_confirms];
    uint256 lastblock = pblockLast ? pblockLast->GetBlockHash() : uint256();

    UniValue ret(UniValue::VOBJ);
    ret.push_back(Pair("transactions", transactions));
    if (include_removed) ret.push_back(Pair("removed", removed));
    ret.push_back(Pair("lastblock", lastblock.GetHex()));

    return ret;
}

UniValue gettransaction(const JSONRPCRequest& request)
{
    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);
    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || request.params.size() < 1 || request.params.size() > 2)
        throw std::runtime_error(
            "gettransaction \"txid\" ( include_watchonly )\n"
            "\nGet detailed information about in-wallet transaction <txid>\n"
            "\nArguments:\n"
            "1. \"txid\"                  (string, required) The transaction id\n"
            "2. \"include_watchonly\"     (bool, optional, default=false) Whether to include watch-only addresses in balance calculation and details[]\n"
            "\nResult:\n"
            "{\n"
            "  \"amount\" : x.xxx,        (numeric) The transaction amount in " + CURRENCY_UNIT + "\n"
            "  \"fee\": x.xxx,            (numeric) The amount of the fee in " + CURRENCY_UNIT + ". This is negative and only available for the \n"
            "                              'send' category of transactions.\n"
            "  \"confirmations\" : n,     (numeric) The number of confirmations\n"
            "  \"blockhash\" : \"hash\",  (string) The block hash\n"
            "  \"blockindex\" : xx,       (numeric) The index of the transaction in the block that includes it\n"
            "  \"blocktime\" : ttt,       (numeric) The time in seconds since epoch (1 Jan 1970 GMT)\n"
            "  \"txid\" : \"transactionid\",   (string) The transaction id.\n"
            "  \"time\" : ttt,            (numeric) The transaction time in seconds since epoch (1 Jan 1970 GMT)\n"
            "  \"timereceived\" : ttt,    (numeric) The time received in seconds since epoch (1 Jan 1970 GMT)\n"
            "  \"bip125-replaceable\": \"yes|no|unknown\",  (string) Whether this transaction could be replaced due to BIP125 (replace-by-fee);\n"
            "                                                   may be unknown for unconfirmed transactions not in the mempool\n"
            "  \"details\" : [\n"
            "    {\n"
            "      \"account\" : \"accountname\",      (string) DEPRECATED. The account name involved in the transaction, can be \"\" for the default account.\n"
            "      \"address\" : \"address\",          (string) The nix address involved in the transaction\n"
            "      \"category\" : \"send|receive\",    (string) The category, either 'send' or 'receive'\n"
            "      \"amount\" : x.xxx,                 (numeric) The amount in " + CURRENCY_UNIT + "\n"
            "      \"label\" : \"label\",              (string) A comment for the address/transaction, if any\n"
            "      \"vout\" : n,                       (numeric) the vout value\n"
            "      \"fee\": x.xxx,                     (numeric) The amount of the fee in " + CURRENCY_UNIT + ". This is negative and only available for the \n"
            "                                           'send' category of transactions.\n"
            "      \"abandoned\": xxx                  (bool) 'true' if the transaction has been abandoned (inputs are respendable). Only available for the \n"
            "                                           'send' category of transactions.\n"
            "    }\n"
            "    ,...\n"
            "  ],\n"
            "  \"hex\" : \"data\"         (string) Raw data for transaction\n"
            "}\n"

            "\nExamples:\n"
            + HelpExampleCli("gettransaction", "\"1075db55d416d3ca199f55b6084e2115b9345e16c5cf302fc80e9d5fbf5d48d\"")
            + HelpExampleCli("gettransaction", "\"1075db55d416d3ca199f55b6084e2115b9345e16c5cf302fc80e9d5fbf5d48d\" true")
            + HelpExampleRpc("gettransaction", "\"1075db55d416d3ca199f55b6084e2115b9345e16c5cf302fc80e9d5fbf5d48d\"")
        );

    ObserveSafeMode();

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pwallet->BlockUntilSyncedToCurrentChain();

    LOCK2(cs_main, pwallet->cs_wallet);

    uint256 hash;
    hash.SetHex(request.params[0].get_str());

    isminefilter filter = ISMINE_SPENDABLE;
    if(!request.params[1].isNull())
        if(request.params[1].get_bool())
            filter = filter | ISMINE_WATCH_ONLY;

    UniValue entry(UniValue::VOBJ);
    auto it = pwallet->mapWallet.find(hash);
    if (it == pwallet->mapWallet.end()) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid or non-wallet transaction id");
    }
    const CWalletTx& wtx = it->second;

    CAmount nCredit = wtx.GetCredit(filter);
    CAmount nDebit = wtx.GetDebit(filter);
    CAmount nNet = nCredit - nDebit;
    CAmount nFee = (wtx.IsFromMe(filter) ? wtx.tx->GetValueOut() - nDebit : 0);

    entry.push_back(Pair("amount", ValueFromAmount(nNet - nFee)));
    if (wtx.IsFromMe(filter))
        entry.push_back(Pair("fee", ValueFromAmount(nFee)));

    WalletTxToJSON(wtx, entry);

    UniValue details(UniValue::VARR);
    ListTransactions(pwallet, wtx, "*", 0, false, details, filter);
    entry.push_back(Pair("details", details));

    std::string strHex = EncodeHexTx(*wtx.tx, RPCSerializationFlags());
    entry.push_back(Pair("hex", strHex));

    return entry;
}

UniValue abandontransaction(const JSONRPCRequest& request)
{
    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);
    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || request.params.size() != 1)
        throw std::runtime_error(
            "abandontransaction \"txid\"\n"
            "\nMark in-wallet transaction <txid> as abandoned\n"
            "This will mark this transaction and all its in-wallet descendants as abandoned which will allow\n"
            "for their inputs to be respent.  It can be used to replace \"stuck\" or evicted transactions.\n"
            "It only works on transactions which are not included in a block and are not currently in the mempool.\n"
            "It has no effect on transactions which are already conflicted or abandoned.\n"
            "\nArguments:\n"
            "1. \"txid\"    (string, required) The transaction id\n"
            "\nResult:\n"
            "\nExamples:\n"
            + HelpExampleCli("abandontransaction", "\"1075db55d416d3ca199f55b6084e2115b9345e16c5cf302fc80e9d5fbf5d48d\"")
            + HelpExampleRpc("abandontransaction", "\"1075db55d416d3ca199f55b6084e2115b9345e16c5cf302fc80e9d5fbf5d48d\"")
        );

    ObserveSafeMode();

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pwallet->BlockUntilSyncedToCurrentChain();

    LOCK2(cs_main, pwallet->cs_wallet);

    uint256 hash;
    hash.SetHex(request.params[0].get_str());

    if (!pwallet->mapWallet.count(hash)) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid or non-wallet transaction id");
    }
    if (!pwallet->AbandonTransaction(hash)) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Transaction not eligible for abandonment");
    }

    return NullUniValue;
}


UniValue backupwallet(const JSONRPCRequest& request)
{
    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);
    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || request.params.size() != 1)
        throw std::runtime_error(
            "backupwallet \"destination\"\n"
            "\nSafely copies current wallet file to destination, which can be a directory or a path with filename.\n"
            "\nArguments:\n"
            "1. \"destination\"   (string) The destination directory or file\n"
            "\nExamples:\n"
            + HelpExampleCli("backupwallet", "\"backup.dat\"")
            + HelpExampleRpc("backupwallet", "\"backup.dat\"")
        );

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pwallet->BlockUntilSyncedToCurrentChain();

    LOCK2(cs_main, pwallet->cs_wallet);

    std::string strDest = request.params[0].get_str();
    if (!pwallet->BackupWallet(strDest)) {
        throw JSONRPCError(RPC_WALLET_ERROR, "Error: Wallet backup failed!");
    }

    return NullUniValue;
}


UniValue keypoolrefill(const JSONRPCRequest& request)
{
    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);
    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || request.params.size() > 1)
        throw std::runtime_error(
            "keypoolrefill ( newsize )\n"
            "\nFills the keypool."
            + HelpRequiringPassphrase(pwallet) + "\n"
            "\nArguments\n"
            "1. newsize     (numeric, optional, default=100) The new keypool size\n"
            "\nExamples:\n"
            + HelpExampleCli("keypoolrefill", "")
            + HelpExampleRpc("keypoolrefill", "")
        );

    LOCK2(cs_main, pwallet->cs_wallet);

    // 0 is interpreted by TopUpKeyPool() as the default keypool size given by -keypool
    unsigned int kpSize = 0;
    if (!request.params[0].isNull()) {
        if (request.params[0].get_int() < 0)
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter, expected valid size.");
        kpSize = (unsigned int)request.params[0].get_int();
    }

    EnsureWalletIsUnlocked(pwallet);
    pwallet->TopUpKeyPool(kpSize);

    if (pwallet->GetKeyPoolSize() < kpSize) {
        throw JSONRPCError(RPC_WALLET_ERROR, "Error refreshing keypool.");
    }

    return NullUniValue;
}


static void LockWallet(CWallet* pWallet)
{
    LOCK(pWallet->cs_wallet);
    pWallet->nRelockTime = 0;
    pWallet->Lock();
}

UniValue walletpassphrase(const JSONRPCRequest& request)
{
    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);
    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || request.params.size() < 2 || request.params.size() > 3) {
        throw std::runtime_error(
            "walletpassphrase <passphrase> <timeout> [stakingonly]\n"
            "\nStores the wallet decryption key in memory for 'timeout' seconds.\n"
            "This is needed prior to performing transactions related to private keys such as sending " + CURRENCY_UNIT + "\n"
            "\nArguments:\n"
            "1. \"passphrase\"     (string, required) The wallet passphrase\n"
            "2. timeout            (numeric, required) The time to keep the decryption key in seconds. Limited to at most 1073741824 (2^30) seconds.\n"
            "                                          Any value greater than 1073741824 seconds will be set to 1073741824 seconds.\n"
            "3. stakingonly        (bool, optional) If true, sending functions are disabled.\n"
            "\nNote:\n"
            "Issuing the walletpassphrase command while the wallet is already unlocked will set a new unlock\n"
            "time that overrides the old one.\n"
            "If [stakingonly] is true and <timeout> is 0, the wallet will remain unlocked for staking until manually locked again.\n"
            "\nExamples:\n"
            "\nUnlock the wallet for 60 seconds\n"
            + HelpExampleCli("walletpassphrase", "\"my pass phrase\" 60") +
            "\nLock the wallet again (before 60 seconds)\n"
            + HelpExampleCli("walletlock", "") +
            "\nUnlock the wallet to stake\n"
            + HelpExampleCli("walletpassphrase", "\"my pass phrase\" 0 true") +
            "\nAs json rpc call\n"
            + HelpExampleRpc("walletpassphrase", "\"my pass phrase\", 60")
        );
    }

    LOCK(cs_main);

    if (request.fHelp)
        return true;
    if (!pwallet->IsCrypted()) {
        throw JSONRPCError(RPC_WALLET_WRONG_ENC_STATE, "Error: running with an unencrypted wallet, but walletpassphrase was called.");
    }

    // Note that the walletpassphrase is stored in request.params[0] which is not mlock()ed
    SecureString strWalletPass;
    strWalletPass.reserve(100);
    // TODO: get rid of this .c_str() by implementing SecureString::operator=(std::string)
    // Alternately, find a way to make request.params[0] mlock()'d to begin with.
    strWalletPass = request.params[0].get_str().c_str();

    // Get the timeout
    int64_t nSleepTime = request.params[1].get_int64();
    // Timeout cannot be negative, otherwise it will relock immediately
    if (nSleepTime < 0) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Timeout cannot be negative.");
    }
    // Clamp timeout to 2^30 seconds
    if (nSleepTime > (int64_t)1 << 30) {
        nSleepTime = (int64_t)1 << 30;
    }

    if (strWalletPass.length() > 0)
    {
        if (!pwallet->Unlock(strWalletPass)) {
            throw JSONRPCError(RPC_WALLET_PASSPHRASE_INCORRECT, "Error: The wallet passphrase entered was incorrect.");
        }
    }
    else
        throw std::runtime_error(
            "walletpassphrase <passphrase> <timeout> [stakingonly]\n"
            "Stores the wallet decryption key in memory for <timeout> seconds.");

    {
        LOCK(pwallet->cs_wallet);
        pwallet->TopUpKeyPool();

        bool fWalletUnlockStakingOnly = false;
        if (request.params.size() > 2)
            fWalletUnlockStakingOnly = request.params[2].get_bool();

        if ((pwallet->IsHDEnabled()))
        {
            LOCK(pwallet->cs_wallet);
            pwallet->fUnlockForStakingOnly = fWalletUnlockStakingOnly;
        }

        // Only allow unlimited timeout (nSleepTime=0) on staking.
        if (nSleepTime > 0 || !fWalletUnlockStakingOnly)
        {
            pwallet->nRelockTime = GetTime() + nSleepTime;
            RPCRunLater(strprintf("lockwallet(%s)", pwallet->GetName()), boost::bind(LockWallet, pwallet), nSleepTime);
        } else
        {
            RPCRunLaterErase(strprintf("lockwallet(%s)", pwallet->GetName()));
            pwallet->nRelockTime = 0;
        }
    }
    return NullUniValue;
}


UniValue walletpassphrasechange(const JSONRPCRequest& request)
{
    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);
    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || request.params.size() != 2) {
        throw std::runtime_error(
            "walletpassphrasechange \"oldpassphrase\" \"newpassphrase\"\n"
            "\nChanges the wallet passphrase from 'oldpassphrase' to 'newpassphrase'.\n"
            "\nArguments:\n"
            "1. \"oldpassphrase\"      (string) The current passphrase\n"
            "2. \"newpassphrase\"      (string) The new passphrase\n"
            "\nExamples:\n"
            + HelpExampleCli("walletpassphrasechange", "\"old one\" \"new one\"")
            + HelpExampleRpc("walletpassphrasechange", "\"old one\", \"new one\"")
        );
    }

    LOCK2(cs_main, pwallet->cs_wallet);

    if (request.fHelp)
        return true;
    if (!pwallet->IsCrypted()) {
        throw JSONRPCError(RPC_WALLET_WRONG_ENC_STATE, "Error: running with an unencrypted wallet, but walletpassphrasechange was called.");
    }

    // TODO: get rid of these .c_str() calls by implementing SecureString::operator=(std::string)
    // Alternately, find a way to make request.params[0] mlock()'d to begin with.
    SecureString strOldWalletPass;
    strOldWalletPass.reserve(100);
    strOldWalletPass = request.params[0].get_str().c_str();

    SecureString strNewWalletPass;
    strNewWalletPass.reserve(100);
    strNewWalletPass = request.params[1].get_str().c_str();

    if (strOldWalletPass.length() < 1 || strNewWalletPass.length() < 1)
        throw std::runtime_error(
            "walletpassphrasechange <oldpassphrase> <newpassphrase>\n"
            "Changes the wallet passphrase from <oldpassphrase> to <newpassphrase>.");

    if (!pwallet->ChangeWalletPassphrase(strOldWalletPass, strNewWalletPass)) {
        throw JSONRPCError(RPC_WALLET_PASSPHRASE_INCORRECT, "Error: The wallet passphrase entered was incorrect.");
    }

    return NullUniValue;
}


UniValue walletlock(const JSONRPCRequest& request)
{
    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);
    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || request.params.size() != 0) {
        throw std::runtime_error(
            "walletlock\n"
            "\nRemoves the wallet encryption key from memory, locking the wallet.\n"
            "After calling this method, you will need to call walletpassphrase again\n"
            "before being able to call any methods which require the wallet to be unlocked.\n"
            "\nExamples:\n"
            "\nSet the passphrase for 2 minutes to perform a transaction\n"
            + HelpExampleCli("walletpassphrase", "\"my pass phrase\" 120") +
            "\nPerform a send (requires passphrase set)\n"
            + HelpExampleCli("sendtoaddress", "\"1M72Sfpbz1BPpXFHz9m3CdqATR44Jvaydd\" 1.0") +
            "\nClear the passphrase since we are done before 2 minutes is up\n"
            + HelpExampleCli("walletlock", "") +
            "\nAs json rpc call\n"
            + HelpExampleRpc("walletlock", "")
        );
    }

    LOCK2(cs_main, pwallet->cs_wallet);

    if (request.fHelp)
        return true;
    if (!pwallet->IsCrypted()) {
        throw JSONRPCError(RPC_WALLET_WRONG_ENC_STATE, "Error: running with an unencrypted wallet, but walletlock was called.");
    }

    pwallet->Lock();
    pwallet->nRelockTime = 0;

    return NullUniValue;
}


UniValue encryptwallet(const JSONRPCRequest& request)
{
    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);
    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || request.params.size() != 1) {
        throw std::runtime_error(
            "encryptwallet \"passphrase\"\n"
            "\nEncrypts the wallet with 'passphrase'. This is for first time encryption.\n"
            "After this, any calls that interact with private keys such as sending or signing \n"
            "will require the passphrase to be set prior the making these calls.\n"
            "Use the walletpassphrase call for this, and then walletlock call.\n"
            "If the wallet is already encrypted, use the walletpassphrasechange call.\n"
            "Note that this will shutdown the server.\n"
            "\nArguments:\n"
            "1. \"passphrase\"    (string) The pass phrase to encrypt the wallet with. It must be at least 1 character, but should be long.\n"
            "\nExamples:\n"
            "\nEncrypt your wallet\n"
            + HelpExampleCli("encryptwallet", "\"my pass phrase\"") +
            "\nNow set the passphrase to use the wallet, such as for signing or sending nix\n"
            + HelpExampleCli("walletpassphrase", "\"my pass phrase\"") +
            "\nNow we can do something like sign\n"
            + HelpExampleCli("signmessage", "\"address\" \"test message\"") +
            "\nNow lock the wallet again by removing the passphrase\n"
            + HelpExampleCli("walletlock", "") +
            "\nAs a json rpc call\n"
            + HelpExampleRpc("encryptwallet", "\"my pass phrase\"")
        );
    }

    LOCK2(cs_main, pwallet->cs_wallet);

    if (request.fHelp)
        return true;
    if (pwallet->IsCrypted()) {
        throw JSONRPCError(RPC_WALLET_WRONG_ENC_STATE, "Error: running with an encrypted wallet, but encryptwallet was called.");
    }

    // TODO: get rid of this .c_str() by implementing SecureString::operator=(std::string)
    // Alternately, find a way to make request.params[0] mlock()'d to begin with.
    SecureString strWalletPass;
    strWalletPass.reserve(100);
    strWalletPass = request.params[0].get_str().c_str();

    if (strWalletPass.length() < 1)
        throw std::runtime_error(
            "encryptwallet <passphrase>\n"
            "Encrypts the wallet with <passphrase>.");

    if (!pwallet->EncryptWallet(strWalletPass)) {
        throw JSONRPCError(RPC_WALLET_ENCRYPTION_FAILED, "Error: Failed to encrypt the wallet.");
    }

    // BDB seems to have a bad habit of writing old data into
    // slack space in .dat files; that is bad if the old data is
    // unencrypted private keys. So:
    StartShutdown();
    return "wallet encrypted; NIX server stopping, restart to run with encrypted wallet. The keypool has been flushed and a new HD seed was generated (if you are using HD). You need to make a new backup.";
}

UniValue lockunspent(const JSONRPCRequest& request)
{
    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);
    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || request.params.size() < 1 || request.params.size() > 2)
        throw std::runtime_error(
            "lockunspent unlock ([{\"txid\":\"txid\",\"vout\":n},...])\n"
            "\nUpdates list of temporarily unspendable outputs.\n"
            "Temporarily lock (unlock=false) or unlock (unlock=true) specified transaction outputs.\n"
            "If no transaction outputs are specified when unlocking then all current locked transaction outputs are unlocked.\n"
            "A locked transaction output will not be chosen by automatic coin selection, when spending nix.\n"
            "Locks are stored in memory only. Nodes start with zero locked outputs, and the locked output list\n"
            "is always cleared (by virtue of process exit) when a node stops or fails.\n"
            "Also see the listunspent call\n"
            "\nArguments:\n"
            "1. unlock            (boolean, required) Whether to unlock (true) or lock (false) the specified transactions\n"
            "2. \"transactions\"  (string, optional) A json array of objects. Each object the txid (string) vout (numeric)\n"
            "     [           (json array of json objects)\n"
            "       {\n"
            "         \"txid\":\"id\",    (string) The transaction id\n"
            "         \"vout\": n         (numeric) The output number\n"
            "       }\n"
            "       ,...\n"
            "     ]\n"

            "\nResult:\n"
            "true|false    (boolean) Whether the command was successful or not\n"

            "\nExamples:\n"
            "\nList the unspent transactions\n"
            + HelpExampleCli("listunspent", "") +
            "\nLock an unspent transaction\n"
            + HelpExampleCli("lockunspent", "false \"[{\\\"txid\\\":\\\"a08e6907dbbd3d809776dbfc5d82e371b764ed838b5655e72f463568df1aadf0\\\",\\\"vout\\\":1}]\"") +
            "\nList the locked transactions\n"
            + HelpExampleCli("listlockunspent", "") +
            "\nUnlock the transaction again\n"
            + HelpExampleCli("lockunspent", "true \"[{\\\"txid\\\":\\\"a08e6907dbbd3d809776dbfc5d82e371b764ed838b5655e72f463568df1aadf0\\\",\\\"vout\\\":1}]\"") +
            "\nAs a json rpc call\n"
            + HelpExampleRpc("lockunspent", "false, \"[{\\\"txid\\\":\\\"a08e6907dbbd3d809776dbfc5d82e371b764ed838b5655e72f463568df1aadf0\\\",\\\"vout\\\":1}]\"")
        );

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pwallet->BlockUntilSyncedToCurrentChain();

    LOCK2(cs_main, pwallet->cs_wallet);

    RPCTypeCheckArgument(request.params[0], UniValue::VBOOL);

    bool fUnlock = request.params[0].get_bool();

    if (request.params[1].isNull()) {
        if (fUnlock)
            pwallet->UnlockAllCoins();
        return true;
    }

    RPCTypeCheckArgument(request.params[1], UniValue::VARR);

    const UniValue& output_params = request.params[1];

    // Create and validate the COutPoints first.

    std::vector<COutPoint> outputs;
    outputs.reserve(output_params.size());

    for (unsigned int idx = 0; idx < output_params.size(); idx++) {
        const UniValue& o = output_params[idx].get_obj();

        RPCTypeCheckObj(o,
            {
                {"txid", UniValueType(UniValue::VSTR)},
                {"vout", UniValueType(UniValue::VNUM)},
            });

        const std::string& txid = find_value(o, "txid").get_str();
        if (!IsHex(txid)) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter, expected hex txid");
        }

        const int nOutput = find_value(o, "vout").get_int();
        if (nOutput < 0) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter, vout must be positive");
        }

        const COutPoint outpt(uint256S(txid), nOutput);

        const auto it = pwallet->mapWallet.find(outpt.hash);
        if (it == pwallet->mapWallet.end()) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter, unknown transaction");
        }

        const CWalletTx& trans = it->second;

        if (outpt.n >= trans.tx->vout.size()) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter, vout index out of bounds");
        }

        if (pwallet->IsSpent(outpt.hash, outpt.n)) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter, expected unspent output");
        }

        const bool is_locked = pwallet->IsLockedCoin(outpt.hash, outpt.n);

        if (fUnlock && !is_locked) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter, expected locked output");
        }

        if (!fUnlock && is_locked) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter, output already locked");
        }

        outputs.push_back(outpt);
    }

    // Atomically set (un)locked status for the outputs.
    for (const COutPoint& outpt : outputs) {
        if (fUnlock) pwallet->UnlockCoin(outpt);
        else pwallet->LockCoin(outpt);
    }

    return true;
}

UniValue listlockunspent(const JSONRPCRequest& request)
{
    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);
    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || request.params.size() > 0)
        throw std::runtime_error(
            "listlockunspent\n"
            "\nReturns list of temporarily unspendable outputs.\n"
            "See the lockunspent call to lock and unlock transactions for spending.\n"
            "\nResult:\n"
            "[\n"
            "  {\n"
            "    \"txid\" : \"transactionid\",     (string) The transaction id locked\n"
            "    \"vout\" : n                      (numeric) The vout value\n"
            "  }\n"
            "  ,...\n"
            "]\n"
            "\nExamples:\n"
            "\nList the unspent transactions\n"
            + HelpExampleCli("listunspent", "") +
            "\nLock an unspent transaction\n"
            + HelpExampleCli("lockunspent", "false \"[{\\\"txid\\\":\\\"a08e6907dbbd3d809776dbfc5d82e371b764ed838b5655e72f463568df1aadf0\\\",\\\"vout\\\":1}]\"") +
            "\nList the locked transactions\n"
            + HelpExampleCli("listlockunspent", "") +
            "\nUnlock the transaction again\n"
            + HelpExampleCli("lockunspent", "true \"[{\\\"txid\\\":\\\"a08e6907dbbd3d809776dbfc5d82e371b764ed838b5655e72f463568df1aadf0\\\",\\\"vout\\\":1}]\"") +
            "\nAs a json rpc call\n"
            + HelpExampleRpc("listlockunspent", "")
        );

    ObserveSafeMode();
    LOCK2(cs_main, pwallet->cs_wallet);

    std::vector<COutPoint> vOutpts;
    pwallet->ListLockedCoins(vOutpts);

    UniValue ret(UniValue::VARR);

    for (COutPoint &outpt : vOutpts) {
        UniValue o(UniValue::VOBJ);

        o.push_back(Pair("txid", outpt.hash.GetHex()));
        o.push_back(Pair("vout", (int)outpt.n));
        ret.push_back(o);
    }

    return ret;
}

UniValue settxfee(const JSONRPCRequest& request)
{
    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);
    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || request.params.size() < 1 || request.params.size() > 1)
        throw std::runtime_error(
            "settxfee amount\n"
            "\nSet the transaction fee per kB. Overwrites the paytxfee parameter.\n"
            "\nArguments:\n"
            "1. amount         (numeric or string, required) The transaction fee in " + CURRENCY_UNIT + "/kB\n"
            "\nResult\n"
            "true|false        (boolean) Returns true if successful\n"
            "\nExamples:\n"
            + HelpExampleCli("settxfee", "0.00001")
            + HelpExampleRpc("settxfee", "0.00001")
        );

    LOCK2(cs_main, pwallet->cs_wallet);

    // Amount
    CAmount nAmount = AmountFromValue(request.params[0]);

    payTxFee = CFeeRate(nAmount, 1000);
    return true;
}

UniValue getwalletinfo(const JSONRPCRequest& request)
{
    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);
    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || request.params.size() != 0)
        throw std::runtime_error(
            "getwalletinfo\n"
            "Returns an object containing various wallet state info.\n"
            "\nResult:\n"
            "{\n"
            "  \"walletname\": xxxxx,             (string) the wallet name\n"
            "  \"walletversion\": xxxxx,          (numeric) the wallet version\n"
            "  \"balance\": xxxxxxx,              (numeric) the total confirmed balance of the wallet in " + CURRENCY_UNIT + "\n"
            "  \"unconfirmed_balance\": xxx,      (numeric) the total unconfirmed balance of the wallet in " + CURRENCY_UNIT + "\n"
            "  \"immature_balance\": xxxxxx,      (numeric) the total immature balance of the wallet in " + CURRENCY_UNIT + "\n"
            "  \"txcount\": xxxxxxx,              (numeric) the total number of transactions in the wallet\n"
            "  \"keypoololdest\": xxxxxx,         (numeric) the timestamp (seconds since Unix epoch) of the oldest pre-generated key in the key pool\n"
            "  \"keypoolsize\": xxxx,             (numeric) how many new keys are pre-generated (only counts external keys)\n"
            "  \"keypoolsize_hd_internal\": xxxx, (numeric) how many new keys are pre-generated for internal use (used for change outputs, only appears if the wallet is using this feature, otherwise external keys are used)\n"
            "  \"unlocked_until\": ttt,           (numeric) the timestamp in seconds since epoch (midnight Jan 1 1970 GMT) that the wallet is unlocked for transfers, or 0 if the wallet is locked\n"
            "  \"paytxfee\": x.xxxx,              (numeric) the transaction fee configuration, set in " + CURRENCY_UNIT + "/kB\n"
            "  \"hdmasterkeyid\": \"<hash160>\"     (string, optional) the Hash160 of the HD master pubkey (only present when HD is enabled)\n"
            "}\n"
            "\nExamples:\n"
            + HelpExampleCli("getwalletinfo", "")
            + HelpExampleRpc("getwalletinfo", "")
        );

    ObserveSafeMode();

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pwallet->BlockUntilSyncedToCurrentChain();

    LOCK2(cs_main, pwallet->cs_wallet);

    UniValue obj(UniValue::VOBJ);

    size_t kpExternalSize = pwallet->KeypoolCountExternalKeys();
    obj.push_back(Pair("walletname", pwallet->GetName()));
    obj.push_back(Pair("walletversion", pwallet->GetVersion()));
    obj.push_back(Pair("balance",       ValueFromAmount(pwallet->GetBalance())));
    obj.push_back(Pair("ghost_vault_legacy",       ValueFromAmount(pwallet->GetGhostBalance(false))));
    obj.push_back(Pair("ghost_vault",       ValueFromAmount(pwallet->GetGhostBalance(true))));
    obj.push_back(Pair("ghost_vault_unconfirmed",       ValueFromAmount(pwallet->GetGhostBalanceUnconfirmed(true))));
    obj.push_back(Pair("unconfirmed_balance", ValueFromAmount(pwallet->GetUnconfirmedBalance())));
    obj.push_back(Pair("immature_balance",    ValueFromAmount(pwallet->GetImmatureBalance())));
    obj.push_back(Pair("coldstake_outputs",    ValueFromAmount(pwallet->CountColdstakeOutputs())));
    obj.push_back(Pair("txcount",       (int)pwallet->mapWallet.size()));
    obj.push_back(Pair("keypoololdest", pwallet->GetOldestKeyPoolTime()));
    obj.push_back(Pair("keypoolsize", (int64_t)kpExternalSize));
    CKeyID masterKeyID = pwallet->GetHDChain().masterKeyID;
    if (!masterKeyID.IsNull() && pwallet->CanSupportFeature(FEATURE_HD_SPLIT)) {
        obj.push_back(Pair("keypoolsize_hd_internal",   (int64_t)(pwallet->GetKeyPoolSize() - kpExternalSize)));
    }

    obj.push_back(Pair("reserve",   ValueFromAmount(pwallet->nReserveBalance)));

    obj.push_back(Pair("encryptionstatus", !pwallet->IsCrypted()
    ? "Unencrypted" : pwallet->IsLocked() ? "Locked" : pwallet->fUnlockForStakingOnly ? "Unlocked, staking only" : "Unlocked"));

    if (pwallet->IsCrypted()) {
        obj.push_back(Pair("unlocked_until", pwallet->nRelockTime));
    }
    obj.push_back(Pair("paytxfee",      ValueFromAmount(payTxFee.GetFeePerK())));
    if (!masterKeyID.IsNull())
         obj.push_back(Pair("hdmasterkeyid", masterKeyID.GetHex()));


    return obj;
}

UniValue listwallets(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() != 0)
        throw std::runtime_error(
            "listwallets\n"
            "Returns a list of currently loaded wallets.\n"
            "For full information on the wallet, use \"getwalletinfo\"\n"
            "\nResult:\n"
            "[                         (json array of strings)\n"
            "  \"walletname\"            (string) the wallet name\n"
            "   ...\n"
            "]\n"
            "\nExamples:\n"
            + HelpExampleCli("listwallets", "")
            + HelpExampleRpc("listwallets", "")
        );

    UniValue obj(UniValue::VARR);

    for (CWalletRef pwallet : vpwallets) {

        if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
            return NullUniValue;
        }

        LOCK(pwallet->cs_wallet);

        obj.push_back(pwallet->GetName());
    }

    return obj;
}

UniValue resendwallettransactions(const JSONRPCRequest& request)
{
    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);
    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || request.params.size() != 0)
        throw std::runtime_error(
            "resendwallettransactions\n"
            "Immediately re-broadcast unconfirmed wallet transactions to all peers.\n"
            "Intended only for testing; the wallet code periodically re-broadcasts\n"
            "automatically.\n"
            "Returns an RPC error if -walletbroadcast is set to false.\n"
            "Returns array of transaction ids that were re-broadcast.\n"
            );

    if (!g_connman)
        throw JSONRPCError(RPC_CLIENT_P2P_DISABLED, "Error: Peer-to-peer functionality missing or disabled");

    LOCK2(cs_main, pwallet->cs_wallet);

    if (!pwallet->GetBroadcastTransactions()) {
        throw JSONRPCError(RPC_WALLET_ERROR, "Error: Wallet transaction broadcasting is disabled with -walletbroadcast");
    }

    std::vector<uint256> txids = pwallet->ResendWalletTransactionsBefore(GetTime(), g_connman.get());
    UniValue result(UniValue::VARR);
    for (const uint256& txid : txids)
    {
        result.push_back(txid.ToString());
    }
    return result;
}

UniValue listunspent(const JSONRPCRequest& request)
{
    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);
    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || request.params.size() > 5)
        throw std::runtime_error(
            "listunspent ( minconf maxconf  [\"addresses\",...] [include_unsafe] [query_options])\n"
            "\nReturns array of unspent transaction outputs\n"
            "with between minconf and maxconf (inclusive) confirmations.\n"
            "Optionally filter to only include txouts paid to specified addresses.\n"
            "\nArguments:\n"
            "1. minconf          (numeric, optional, default=1) The minimum confirmations to filter\n"
            "2. maxconf          (numeric, optional, default=9999999) The maximum confirmations to filter\n"
            "3. \"addresses\"      (string) A json array of nix addresses to filter\n"
            "    [\n"
            "      \"address\"     (string) nix address\n"
            "      ,...\n"
            "    ]\n"
            "4. include_unsafe (bool, optional, default=true) Include outputs that are not safe to spend\n"
            "                  See description of \"safe\" attribute below.\n"
            "5. query_options    (json, optional) JSON with query options\n"
            "    {\n"
            "      \"minimumAmount\"    (numeric or string, default=0) Minimum value of each UTXO in " + CURRENCY_UNIT + "\n"
            "      \"maximumAmount\"    (numeric or string, default=unlimited) Maximum value of each UTXO in " + CURRENCY_UNIT + "\n"
            "      \"maximumCount\"     (numeric or string, default=unlimited) Maximum number of UTXOs\n"
            "      \"minimumSumAmount\" (numeric or string, default=unlimited) Minimum sum value of all UTXOs in " + CURRENCY_UNIT + "\n"
            "    }\n"
            "\nResult\n"
            "[                   (array of json object)\n"
            "  {\n"
            "    \"txid\" : \"txid\",          (string) the transaction id \n"
            "    \"vout\" : n,               (numeric) the vout value\n"
            "    \"address\" : \"address\",    (string) the nix address\n"
            "    \"account\" : \"account\",    (string) DEPRECATED. The associated account, or \"\" for the default account\n"
            "    \"scriptPubKey\" : \"key\",   (string) the script key\n"
            "    \"amount\" : x.xxx,         (numeric) the transaction output amount in " + CURRENCY_UNIT + "\n"
            "    \"confirmations\" : n,      (numeric) The number of confirmations\n"
            "    \"redeemScript\" : n        (string) The redeemScript if scriptPubKey is P2SH\n"
            "    \"spendable\" : xxx,        (bool) Whether we have the private keys to spend this output\n"
            "    \"solvable\" : xxx,         (bool) Whether we know how to spend this output, ignoring the lack of keys\n"
            "    \"safe\" : xxx              (bool) Whether this output is considered safe to spend. Unconfirmed transactions\n"
            "                              from outside keys and unconfirmed replacement transactions are considered unsafe\n"
            "                              and are not eligible for spending by fundrawtransaction and sendtoaddress.\n"
            "  }\n"
            "  ,...\n"
            "]\n"

            "\nExamples\n"
            + HelpExampleCli("listunspent", "")
            + HelpExampleCli("listunspent", "6 9999999 \"[\\\"1PGFqEzfmQch1gKD3ra4k18PNj3tTUUSqg\\\",\\\"1LtvqCaApEdUGFkpKMM4MstjcaL4dKg8SP\\\"]\"")
            + HelpExampleRpc("listunspent", "6, 9999999 \"[\\\"1PGFqEzfmQch1gKD3ra4k18PNj3tTUUSqg\\\",\\\"1LtvqCaApEdUGFkpKMM4MstjcaL4dKg8SP\\\"]\"")
            + HelpExampleCli("listunspent", "6 9999999 '[]' true '{ \"minimumAmount\": 0.005 }'")
            + HelpExampleRpc("listunspent", "6, 9999999, [] , true, { \"minimumAmount\": 0.005 } ")
        );

    ObserveSafeMode();

    int nMinDepth = 1;
    if (!request.params[0].isNull()) {
        RPCTypeCheckArgument(request.params[0], UniValue::VNUM);
        nMinDepth = request.params[0].get_int();
    }

    int nMaxDepth = 9999999;
    if (!request.params[1].isNull()) {
        RPCTypeCheckArgument(request.params[1], UniValue::VNUM);
        nMaxDepth = request.params[1].get_int();
    }

    std::set<CTxDestination> destinations;
    if (!request.params[2].isNull()) {
        RPCTypeCheckArgument(request.params[2], UniValue::VARR);
        UniValue inputs = request.params[2].get_array();
        for (unsigned int idx = 0; idx < inputs.size(); idx++) {
            const UniValue& input = inputs[idx];
            CTxDestination dest = DecodeDestination(input.get_str());
            if (!IsValidDestination(dest)) {
                throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, std::string("Invalid NIX address: ") + input.get_str());
            }
            if (!destinations.insert(dest).second) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, std::string("Invalid parameter, duplicated address: ") + input.get_str());
            }
        }
    }

    bool include_unsafe = true;
    if (!request.params[3].isNull()) {
        RPCTypeCheckArgument(request.params[3], UniValue::VBOOL);
        include_unsafe = request.params[3].get_bool();
    }

    CAmount nMinimumAmount = 0;
    CAmount nMaximumAmount = MAX_MONEY;
    CAmount nMinimumSumAmount = MAX_MONEY;
    uint64_t nMaximumCount = 0;

    if (!request.params[4].isNull()) {
        const UniValue& options = request.params[4].get_obj();

        if (options.exists("minimumAmount"))
            nMinimumAmount = AmountFromValue(options["minimumAmount"]);

        if (options.exists("maximumAmount"))
            nMaximumAmount = AmountFromValue(options["maximumAmount"]);

        if (options.exists("minimumSumAmount"))
            nMinimumSumAmount = AmountFromValue(options["minimumSumAmount"]);

        if (options.exists("maximumCount"))
            nMaximumCount = options["maximumCount"].get_int64();
    }

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pwallet->BlockUntilSyncedToCurrentChain();

    UniValue results(UniValue::VARR);
    std::vector<COutput> vecOutputs;
    LOCK2(cs_main, pwallet->cs_wallet);

    pwallet->AvailableCoins(vecOutputs, !include_unsafe, nullptr, nMinimumAmount, nMaximumAmount, nMinimumSumAmount, nMaximumCount, nMinDepth, nMaxDepth);
    for (const COutput& out : vecOutputs) {
        CTxDestination address;
        const CScript& scriptPubKey = out.tx->tx->vout[out.i].scriptPubKey;
        bool fValidAddress = ExtractDestination(scriptPubKey, address);

        if (destinations.size() && (!fValidAddress || !destinations.count(address)))
            continue;

        UniValue entry(UniValue::VOBJ);
        entry.push_back(Pair("txid", out.tx->GetHash().GetHex()));
        entry.push_back(Pair("vout", out.i));

        if (fValidAddress) {
            entry.push_back(Pair("address", EncodeDestination(address)));

            if (pwallet->mapAddressBook.count(address)) {
                entry.push_back(Pair("account", pwallet->mapAddressBook[address].name));
            }

            if (scriptPubKey.IsPayToScriptHash()) {
                const CScriptID& hash = boost::get<CScriptID>(address);
                CScript redeemScript;
                if (pwallet->GetCScript(hash, redeemScript)) {
                    entry.push_back(Pair("redeemScript", HexStr(redeemScript.begin(), redeemScript.end())));
                }
            }
        }

        entry.push_back(Pair("scriptPubKey", HexStr(scriptPubKey.begin(), scriptPubKey.end())));
        entry.push_back(Pair("amount", ValueFromAmount(out.tx->tx->vout[out.i].nValue)));
        entry.push_back(Pair("confirmations", out.nDepth));
        entry.push_back(Pair("spendable", out.fSpendable));
        entry.push_back(Pair("solvable", out.fSolvable));
        entry.push_back(Pair("safe", out.fSafe));
        results.push_back(entry);
    }

    return results;
}

UniValue fundrawtransaction(const JSONRPCRequest& request)
{
    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);
    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || request.params.size() < 1 || request.params.size() > 3)
        throw std::runtime_error(
                            "fundrawtransaction \"hexstring\" ( options iswitness )\n"
                            "\nAdd inputs to a transaction until it has enough in value to meet its out value.\n"
                            "This will not modify existing inputs, and will add at most one change output to the outputs.\n"
                            "No existing outputs will be modified unless \"subtractFeeFromOutputs\" is specified.\n"
                            "Note that inputs which were signed may need to be resigned after completion since in/outputs have been added.\n"
                            "The inputs added will not be signed, use signrawtransaction for that.\n"
                            "Note that all existing inputs must have their previous output transaction be in the wallet.\n"
                            "Note that all inputs selected must be of standard form and P2SH scripts must be\n"
                            "in the wallet using importaddress or addmultisigaddress (to calculate fees).\n"
                            "You can see whether this is the case by checking the \"solvable\" field in the listunspent output.\n"
                            "Only pay-to-pubkey, multisig, and P2SH versions thereof are currently supported for watch-only\n"
                            "\nArguments:\n"
                            "1. \"hexstring\"           (string, required) The hex string of the raw transaction\n"
                            "2. options                 (object, optional)\n"
                            "   {\n"
                            "     \"changeAddress\"          (string, optional, default pool address) The nix address to receive the change\n"
                            "     \"changePosition\"         (numeric, optional, default random) The index of the change output\n"
                            "     \"change_type\"            (string, optional) The output type to use. Only valid if changeAddress is not specified. Options are \"legacy\", \"p2sh-segwit\", and \"bech32\". Default is set by -changetype.\n"
                            "     \"includeWatching\"        (boolean, optional, default false) Also select inputs which are watch only\n"
                            "     \"lockUnspents\"           (boolean, optional, default false) Lock selected unspent outputs\n"
                            "     \"feeRate\"                (numeric, optional, default not set: makes wallet determine the fee) Set a specific fee rate in " + CURRENCY_UNIT + "/kB\n"
                            "     \"subtractFeeFromOutputs\" (array, optional) A json array of integers.\n"
                            "                              The fee will be equally deducted from the amount of each specified output.\n"
                            "                              The outputs are specified by their zero-based index, before any change output is added.\n"
                            "                              Those recipients will receive less nix than you enter in their corresponding amount field.\n"
                            "                              If no outputs are specified here, the sender pays the fee.\n"
                            "                                  [vout_index,...]\n"
                            "     \"replaceable\"            (boolean, optional) Marks this transaction as BIP125 replaceable.\n"
                            "                              Allows this transaction to be replaced by a transaction with higher fees\n"
                            "     \"conf_target\"            (numeric, optional) Confirmation target (in blocks)\n"
                            "     \"estimate_mode\"          (string, optional, default=UNSET) The fee estimate mode, must be one of:\n"
                            "         \"UNSET\"\n"
                            "         \"ECONOMICAL\"\n"
                            "         \"CONSERVATIVE\"\n"
                            "   }\n"
                            "                         for backward compatibility: passing in a true instead of an object will result in {\"includeWatching\":true}\n"
                            "3. iswitness               (boolean, optional) Whether the transaction hex is a serialized witness transaction \n"
                            "                              If iswitness is not present, heuristic tests will be used in decoding\n"

                            "\nResult:\n"
                            "{\n"
                            "  \"hex\":       \"value\", (string)  The resulting raw transaction (hex-encoded string)\n"
                            "  \"fee\":       n,         (numeric) Fee in " + CURRENCY_UNIT + " the resulting transaction pays\n"
                            "  \"changepos\": n          (numeric) The position of the added change output, or -1\n"
                            "}\n"
                            "\nExamples:\n"
                            "\nCreate a transaction with no inputs\n"
                            + HelpExampleCli("createrawtransaction", "\"[]\" \"{\\\"myaddress\\\":0.01}\"") +
                            "\nAdd sufficient unsigned inputs to meet the output value\n"
                            + HelpExampleCli("fundrawtransaction", "\"rawtransactionhex\"") +
                            "\nSign the transaction\n"
                            + HelpExampleCli("signrawtransaction", "\"fundedtransactionhex\"") +
                            "\nSend the transaction\n"
                            + HelpExampleCli("sendrawtransaction", "\"signedtransactionhex\"")
                            );

    ObserveSafeMode();
    RPCTypeCheck(request.params, {UniValue::VSTR});

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pwallet->BlockUntilSyncedToCurrentChain();

    CCoinControl coinControl;
    int changePosition = -1;
    bool lockUnspents = false;
    UniValue subtractFeeFromOutputs;
    std::set<int> setSubtractFeeFromOutputs;

    if (!request.params[1].isNull()) {
      if (request.params[1].type() == UniValue::VBOOL) {
        // backward compatibility bool only fallback
        coinControl.fAllowWatchOnly = request.params[1].get_bool();
      }
      else {
        RPCTypeCheck(request.params, {UniValue::VSTR, UniValue::VOBJ, UniValue::VBOOL});

        UniValue options = request.params[1];

        RPCTypeCheckObj(options,
            {
                {"changeAddress", UniValueType(UniValue::VSTR)},
                {"changePosition", UniValueType(UniValue::VNUM)},
                {"change_type", UniValueType(UniValue::VSTR)},
                {"includeWatching", UniValueType(UniValue::VBOOL)},
                {"lockUnspents", UniValueType(UniValue::VBOOL)},
                {"reserveChangeKey", UniValueType(UniValue::VBOOL)}, // DEPRECATED (and ignored), should be removed in 0.16 or so.
                {"feeRate", UniValueType()}, // will be checked below
                {"subtractFeeFromOutputs", UniValueType(UniValue::VARR)},
                {"replaceable", UniValueType(UniValue::VBOOL)},
                {"conf_target", UniValueType(UniValue::VNUM)},
                {"estimate_mode", UniValueType(UniValue::VSTR)},
            },
            true, true);

        if (options.exists("changeAddress")) {
            CTxDestination dest = DecodeDestination(options["changeAddress"].get_str());

            if (!IsValidDestination(dest)) {
                throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "changeAddress must be a valid nix address");
            }

            coinControl.destChange = dest;
        }

        if (options.exists("changePosition"))
            changePosition = options["changePosition"].get_int();

        if (options.exists("change_type")) {
            if (options.exists("changeAddress")) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Cannot specify both changeAddress and address_type options");
            }
            coinControl.change_type = ParseOutputType(options["change_type"].get_str(), coinControl.change_type);
            if (coinControl.change_type == OUTPUT_TYPE_NONE) {
                throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, strprintf("Unknown change type '%s'", options["change_type"].get_str()));
            }
        }

        if (options.exists("includeWatching"))
            coinControl.fAllowWatchOnly = options["includeWatching"].get_bool();

        if (options.exists("lockUnspents"))
            lockUnspents = options["lockUnspents"].get_bool();

        if (options.exists("feeRate"))
        {
            coinControl.m_feerate = CFeeRate(AmountFromValue(options["feeRate"]));
            coinControl.fOverrideFeeRate = true;
        }

        if (options.exists("subtractFeeFromOutputs"))
            subtractFeeFromOutputs = options["subtractFeeFromOutputs"].get_array();

        if (options.exists("replaceable")) {
            coinControl.signalRbf = options["replaceable"].get_bool();
        }
        if (options.exists("conf_target")) {
            if (options.exists("feeRate")) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Cannot specify both conf_target and feeRate");
            }
            coinControl.m_confirm_target = ParseConfirmTarget(options["conf_target"]);
        }
        if (options.exists("estimate_mode")) {
            if (options.exists("feeRate")) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Cannot specify both estimate_mode and feeRate");
            }
            if (!FeeModeFromString(options["estimate_mode"].get_str(), coinControl.m_fee_mode)) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid estimate_mode parameter");
            }
        }
      }
    }

    // parse hex string from parameter
    CMutableTransaction tx;
    bool try_witness = request.params[2].isNull() ? true : request.params[2].get_bool();
    bool try_no_witness = request.params[2].isNull() ? true : !request.params[2].get_bool();
    if (!DecodeHexTx(tx, request.params[0].get_str(), try_no_witness, try_witness)) {
        throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "TX decode failed");
    }

    if (tx.vout.size() == 0)
        throw JSONRPCError(RPC_INVALID_PARAMETER, "TX must have at least one output");

    if (changePosition != -1 && (changePosition < 0 || (unsigned int)changePosition > tx.vout.size()))
        throw JSONRPCError(RPC_INVALID_PARAMETER, "changePosition out of bounds");

    for (unsigned int idx = 0; idx < subtractFeeFromOutputs.size(); idx++) {
        int pos = subtractFeeFromOutputs[idx].get_int();
        if (setSubtractFeeFromOutputs.count(pos))
            throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Invalid parameter, duplicated position: %d", pos));
        if (pos < 0)
            throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Invalid parameter, negative position: %d", pos));
        if (pos >= int(tx.vout.size()))
            throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Invalid parameter, position too large: %d", pos));
        setSubtractFeeFromOutputs.insert(pos);
    }

    CAmount nFeeOut;
    std::string strFailReason;

    if (!pwallet->FundTransaction(tx, nFeeOut, changePosition, strFailReason, lockUnspents, setSubtractFeeFromOutputs, coinControl)) {
        throw JSONRPCError(RPC_WALLET_ERROR, strFailReason);
    }

    UniValue result(UniValue::VOBJ);
    result.push_back(Pair("hex", EncodeHexTx(tx)));
    result.push_back(Pair("changepos", changePosition));
    result.push_back(Pair("fee", ValueFromAmount(nFeeOut)));

    return result;
}

UniValue bumpfee(const JSONRPCRequest& request)
{
    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() < 1 || request.params.size() > 2) {
        throw std::runtime_error(
            "bumpfee \"txid\" ( options ) \n"
            "\nBumps the fee of an opt-in-RBF transaction T, replacing it with a new transaction B.\n"
            "An opt-in RBF transaction with the given txid must be in the wallet.\n"
            "The command will pay the additional fee by decreasing (or perhaps removing) its change output.\n"
            "If the change output is not big enough to cover the increased fee, the command will currently fail\n"
            "instead of adding new inputs to compensate. (A future implementation could improve this.)\n"
            "The command will fail if the wallet or mempool contains a transaction that spends one of T's outputs.\n"
            "By default, the new fee will be calculated automatically using estimatefee.\n"
            "The user can specify a confirmation target for estimatefee.\n"
            "Alternatively, the user can specify totalFee, or use RPC settxfee to set a higher fee rate.\n"
            "At a minimum, the new fee rate must be high enough to pay an additional new relay fee (incrementalfee\n"
            "returned by getnetworkinfo) to enter the node's mempool.\n"
            "\nArguments:\n"
            "1. txid                  (string, required) The txid to be bumped\n"
            "2. options               (object, optional)\n"
            "   {\n"
            "     \"confTarget\"        (numeric, optional) Confirmation target (in blocks)\n"
            "     \"totalFee\"          (numeric, optional) Total fee (NOT feerate) to pay, in satoshis.\n"
            "                         In rare cases, the actual fee paid might be slightly higher than the specified\n"
            "                         totalFee if the tx change output has to be removed because it is too close to\n"
            "                         the dust threshold.\n"
            "     \"replaceable\"       (boolean, optional, default true) Whether the new transaction should still be\n"
            "                         marked bip-125 replaceable. If true, the sequence numbers in the transaction will\n"
            "                         be left unchanged from the original. If false, any input sequence numbers in the\n"
            "                         original transaction that were less than 0xfffffffe will be increased to 0xfffffffe\n"
            "                         so the new transaction will not be explicitly bip-125 replaceable (though it may\n"
            "                         still be replaceable in practice, for example if it has unconfirmed ancestors which\n"
            "                         are replaceable).\n"
            "     \"estimate_mode\"     (string, optional, default=UNSET) The fee estimate mode, must be one of:\n"
            "         \"UNSET\"\n"
            "         \"ECONOMICAL\"\n"
            "         \"CONSERVATIVE\"\n"
            "   }\n"
            "\nResult:\n"
            "{\n"
            "  \"txid\":    \"value\",   (string)  The id of the new transaction\n"
            "  \"origfee\":  n,         (numeric) Fee of the replaced transaction\n"
            "  \"fee\":      n,         (numeric) Fee of the new transaction\n"
            "  \"errors\":  [ str... ] (json array of strings) Errors encountered during processing (may be empty)\n"
            "}\n"
            "\nExamples:\n"
            "\nBump the fee, get the new transaction\'s txid\n" +
            HelpExampleCli("bumpfee", "<txid>"));
    }

    RPCTypeCheck(request.params, {UniValue::VSTR, UniValue::VOBJ});
    uint256 hash;
    hash.SetHex(request.params[0].get_str());

    // optional parameters
    CAmount totalFee = 0;
    CCoinControl coin_control;
    coin_control.signalRbf = true;
    if (!request.params[1].isNull()) {
        UniValue options = request.params[1];
        RPCTypeCheckObj(options,
            {
                {"confTarget", UniValueType(UniValue::VNUM)},
                {"totalFee", UniValueType(UniValue::VNUM)},
                {"replaceable", UniValueType(UniValue::VBOOL)},
                {"estimate_mode", UniValueType(UniValue::VSTR)},
            },
            true, true);

        if (options.exists("confTarget") && options.exists("totalFee")) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "confTarget and totalFee options should not both be set. Please provide either a confirmation target for fee estimation or an explicit total fee for the transaction.");
        } else if (options.exists("confTarget")) { // TODO: alias this to conf_target
            coin_control.m_confirm_target = ParseConfirmTarget(options["confTarget"]);
        } else if (options.exists("totalFee")) {
            totalFee = options["totalFee"].get_int64();
            if (totalFee <= 0) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Invalid totalFee %s (must be greater than 0)", FormatMoney(totalFee)));
            }
        }

        if (options.exists("replaceable")) {
            coin_control.signalRbf = options["replaceable"].get_bool();
        }
        if (options.exists("estimate_mode")) {
            if (!FeeModeFromString(options["estimate_mode"].get_str(), coin_control.m_fee_mode)) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid estimate_mode parameter");
            }
        }
    }

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pwallet->BlockUntilSyncedToCurrentChain();

    LOCK2(cs_main, pwallet->cs_wallet);
    EnsureWalletIsUnlocked(pwallet);


    std::vector<std::string> errors;
    CAmount old_fee;
    CAmount new_fee;
    CMutableTransaction mtx;
    feebumper::Result res = feebumper::CreateTransaction(pwallet, hash, coin_control, totalFee, errors, old_fee, new_fee, mtx);
    if (res != feebumper::Result::OK) {
        switch(res) {
            case feebumper::Result::INVALID_ADDRESS_OR_KEY:
                throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, errors[0]);
                break;
            case feebumper::Result::INVALID_REQUEST:
                throw JSONRPCError(RPC_INVALID_REQUEST, errors[0]);
                break;
            case feebumper::Result::INVALID_PARAMETER:
                throw JSONRPCError(RPC_INVALID_PARAMETER, errors[0]);
                break;
            case feebumper::Result::WALLET_ERROR:
                throw JSONRPCError(RPC_WALLET_ERROR, errors[0]);
                break;
            default:
                throw JSONRPCError(RPC_MISC_ERROR, errors[0]);
                break;
        }
    }

    // sign bumped transaction
    if (!feebumper::SignTransaction(pwallet, mtx)) {
        throw JSONRPCError(RPC_WALLET_ERROR, "Can't sign transaction.");
    }
    // commit the bumped transaction
    uint256 txid;
    if (feebumper::CommitTransaction(pwallet, hash, std::move(mtx), errors, txid) != feebumper::Result::OK) {
        throw JSONRPCError(RPC_WALLET_ERROR, errors[0]);
    }
    UniValue result(UniValue::VOBJ);
    result.push_back(Pair("txid", txid.GetHex()));
    result.push_back(Pair("origfee", ValueFromAmount(old_fee)));
    result.push_back(Pair("fee", ValueFromAmount(new_fee)));
    UniValue result_errors(UniValue::VARR);
    for (const std::string& error : errors) {
        result_errors.push_back(error);
    }
    result.push_back(Pair("errors", result_errors));

    return result;
}

UniValue generate(const JSONRPCRequest& request)
{
    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || request.params.size() < 1 || request.params.size() > 2) {
        throw std::runtime_error(
            "generate nblocks ( maxtries )\n"
            "\nMine up to nblocks blocks immediately (before the RPC call returns) to an address in the wallet.\n"
            "\nArguments:\n"
            "1. nblocks      (numeric, required) How many blocks are generated immediately.\n"
            "2. maxtries     (numeric, optional) How many iterations to try (default = 1000000).\n"
            "\nResult:\n"
            "[ blockhashes ]     (array) hashes of blocks generated\n"
            "\nExamples:\n"
            "\nGenerate 11 blocks\n"
            + HelpExampleCli("generate", "11")
        );
    }

    int num_generate = request.params[0].get_int();
    uint64_t max_tries = 1000000;
    if (!request.params[1].isNull()) {
        max_tries = request.params[1].get_int();
    }

    std::shared_ptr<CReserveScript> coinbaseScript;
    pwallet->GetScriptForMining(coinbaseScript);

    // If the keypool is exhausted, no script is returned at all.  Catch this.
    if (!coinbaseScript) {
        throw JSONRPCError(RPC_WALLET_KEYPOOL_RAN_OUT, "Error: Keypool ran out, please call keypoolrefill first");
    }

    //throw an error if no script was provided
    if (coinbaseScript->reserveScript.empty()) {
        throw JSONRPCError(RPC_INTERNAL_ERROR, "No coinbase script available");
    }

    return generateBlocks(coinbaseScript, num_generate, max_tries, true);
}

UniValue rescanblockchain(const JSONRPCRequest& request)
{
    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);
    if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
        return NullUniValue;
    }

    if (request.fHelp || request.params.size() > 2) {
        throw std::runtime_error(
            "rescanblockchain (\"start_height\") (\"stop_height\")\n"
            "\nRescan the local blockchain for wallet related transactions.\n"
            "\nArguments:\n"
            "1. \"start_height\"    (numeric, optional) block height where the rescan should start\n"
            "2. \"stop_height\"     (numeric, optional) the last block height that should be scanned\n"
            "\nResult:\n"
            "{\n"
            "  \"start_height\"     (numeric) The block height where the rescan has started. If omitted, rescan started from the genesis block.\n"
            "  \"stop_height\"      (numeric) The height of the last rescanned block. If omitted, rescan stopped at the chain tip.\n"
            "}\n"
            "\nExamples:\n"
            + HelpExampleCli("rescanblockchain", "100000 120000")
            + HelpExampleRpc("rescanblockchain", "100000, 120000")
            );
    }

    WalletRescanReserver reserver(pwallet);
    if (!reserver.reserve()) {
        throw JSONRPCError(RPC_WALLET_ERROR, "Wallet is currently rescanning. Abort existing rescan or wait.");
    }

    CBlockIndex *pindexStart = nullptr;
    CBlockIndex *pindexStop = nullptr;
    CBlockIndex *pChainTip = nullptr;
    {
        LOCK(cs_main);
        pindexStart = chainActive.Genesis();
        pChainTip = chainActive.Tip();

        if (!request.params[0].isNull()) {
            pindexStart = chainActive[request.params[0].get_int()];
            if (!pindexStart) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid start_height");
            }
        }

        if (!request.params[1].isNull()) {
            pindexStop = chainActive[request.params[1].get_int()];
            if (!pindexStop) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid stop_height");
            }
            else if (pindexStop->nHeight < pindexStart->nHeight) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "stop_height must be greater then start_height");
            }
        }
    }

    // We can't rescan beyond non-pruned blocks, stop and throw an error
    if (fPruneMode) {
        LOCK(cs_main);
        CBlockIndex *block = pindexStop ? pindexStop : pChainTip;
        while (block && block->nHeight >= pindexStart->nHeight) {
            if (!(block->nStatus & BLOCK_HAVE_DATA)) {
                throw JSONRPCError(RPC_MISC_ERROR, "Can't rescan beyond pruned data. Use RPC call getblockchaininfo to determine your pruned height.");
            }
            block = block->pprev;
        }
    }

    CBlockIndex *stopBlock = pwallet->ScanForWalletTransactions(pindexStart, pindexStop, reserver, true);
    if (!stopBlock) {
        if (pwallet->IsAbortingRescan()) {
            throw JSONRPCError(RPC_MISC_ERROR, "Rescan aborted.");
        }
        // if we got a nullptr returned, ScanForWalletTransactions did rescan up to the requested stopindex
        stopBlock = pindexStop ? pindexStop : pChainTip;
    }
    else {
        throw JSONRPCError(RPC_MISC_ERROR, "Rescan failed. Potentially corrupted data files.");
    }
    UniValue response(UniValue::VOBJ);
    response.pushKV("start_height", pindexStart->nHeight);
    response.pushKV("stop_height", stopBlock->nHeight);
    return response;
}


//Nix Privacy section

UniValue listunspentmintzerocoins(const JSONRPCRequest& request) {
    if (request.fHelp || request.params.size() > 2)
        throw runtime_error(
                "listunspentmintzerocoins [minconf=1] [maxconf=9999999] \n"
                        "Returns array of unspent transaction outputs\n"
                        "with between minconf and maxconf (inclusive) confirmations.\n"
                        "Results are an array of Objects, each of which has:\n"
                        "{txid, vout, scriptPubKey, amount, confirmations}");

    CWallet * const pwalletMain = GetWalletForJSONRPCRequest(request);

    if (pwalletMain->IsLocked())
        throw JSONRPCError(RPC_WALLET_UNLOCK_NEEDED,
                           "Error: Please enter the wallet passphrase with walletpassphrase first.");

    UniValue options = request.params;

    int nMinDepth = 1;
    if (request.params.size() > 0)
        nMinDepth = request.params[0].get_int();

    int nMaxDepth = 9999999;
    if (request.params.size() > 1)
        nMaxDepth = request.params[1].get_int();

    UniValue results(UniValue::VARR);
    vector <COutput> vecOutputs;
    assert(pwalletMain != NULL);
    pwalletMain->ListAvailableCoinsMintCoins(vecOutputs, false);
    LogPrintf("vecOutputs.size()=%s\n", vecOutputs.size());
    for(const COutput &out: vecOutputs)
    {
        if (out.nDepth < nMinDepth || out.nDepth > nMaxDepth)
            continue;

        int64_t nValue = out.tx->tx->vout[out.i].nValue;
        const CScript &pk = out.tx->tx->vout[out.i].scriptPubKey;
        UniValue entry(UniValue::VOBJ);
        entry.push_back(Pair("txid", out.tx->GetHash().GetHex()));
        entry.push_back(Pair("vout", out.i));
        entry.push_back(Pair("scriptPubKey", HexStr(pk.begin(), pk.end())));
        if (pk.IsPayToScriptHash()) {
            CTxDestination address;
            if (ExtractDestination(pk, address)) {
                const CScriptID &hash = boost::get<CScriptID>(address);
                CScript redeemScript;
                if (pwalletMain->GetCScript(hash, redeemScript))
                    entry.push_back(Pair("redeemScript", HexStr(redeemScript.begin(), redeemScript.end())));
            }
        }
        entry.push_back(Pair("amount", ValueFromAmount(nValue)));
        entry.push_back(Pair("confirmations", out.nDepth));
        results.push_back(entry);
    }

    return results;
}


UniValue ghostamount(const JSONRPCRequest& request)
{
    CWallet *pwalletMain = GetWalletForJSONRPCRequest(request);

    if (request.fHelp || request.params.size() > 2)
        throw runtime_error("ghostamount <amount>(whole numbers only) <commitment key pack>\n" + HelpRequiringPassphrase(pwalletMain));

    if(fDisableZerocoinTransactions)
        throw JSONRPCError(RPC_WALLET_ERROR, "ghosted tranasactions are not currently being accepted");

    int64_t nAmount = request.params[0].get_int64();

    std::vector<CScript> keypack;
    keypack.clear();
    if(!request.params[1].isNull()){
        std::string k = request.params[1].get_str();
        CommitmentKeyPack keys(k);
        if(!keys.IsValidPack())
            throw JSONRPCError(RPC_WALLET_ERROR, "invalid commitment key pack");
        keypack = keys.GetPubCoinPackScript();
    }
    bool strError = pwalletMain->GhostModeMintTrigger(std::to_string(nAmount), keypack);

    if (!strError)
        throw JSONRPCError(RPC_WALLET_ERROR, "ghostamount");

    return "Sucessfully ghosted " + std::to_string(nAmount) +  " NIX";
}

UniValue unghostamount(const JSONRPCRequest& request)
{
    CWallet *pwalletMain = GetWalletForJSONRPCRequest(request);

    if (request.fHelp || request.params.size() == 0 || request.params.size() > 2)
        throw runtime_error("unghostamount <amount>(whole numbers only) <addresstosend>(either address or commitment key pack)\n" + HelpRequiringPassphrase(pwalletMain));

    int nHeight = 0;
    {
        LOCK(cs_main);
        nHeight = chainActive.Height();

    }

    if(nHeight < Params().GetConsensus().nSigmaStartBlock)
        throw JSONRPCError(RPC_WALLET_ERROR, "zerocoin ghosted tranasactions are not currently being accepted");

    int64_t nAmount = request.params[0].get_int64();

    CTxDestination dest;
    std::string toKey = "";
    std::vector <CScript> keyList;
    keyList.clear();
    if (request.params.size() > 1){
        // Address
        toKey = request.params[1].get_str();
        dest = DecodeDestination(toKey);
         if(!IsValidDestination(dest))
            throw JSONRPCError(RPC_WALLET_ERROR, "invalid key");
    }

    if (pwalletMain->IsLocked())
        throw JSONRPCError(RPC_WALLET_UNLOCK_NEEDED,
                           "Error: Please enter the wallet passphrase with walletpassphrase first.");

    std::string strError = pwalletMain->GhostModeSpendTrigger(std::to_string(nAmount), toKey, keyList);


    return strError;
}

UniValue mintzerocoin(const JSONRPCRequest& request)
{

    CWallet *pwalletMain = GetWalletForJSONRPCRequest(request);

    if (request.fHelp || request.params.size() > 1)
        throw runtime_error("mintzerocoin <amount>(1,5,10,50,100,500,1000,5000)\n" + HelpRequiringPassphrase(pwalletMain));


    int64_t nAmount = 0;
    libzerocoin::CoinDenomination denomination;
    // Amount
    if (request.params[0].get_real() == 1.0) {
        denomination = libzerocoin::ZQ_ONE;
        nAmount = AmountFromValue(request.params[0]);
    } else if (request.params[0].get_real() == 5.0) {
        denomination = libzerocoin::ZQ_FIVE;
        nAmount = AmountFromValue(request.params[0]);
    } else if (request.params[0].get_real() == 10.0) {
        denomination = libzerocoin::ZQ_TEN;
        nAmount = AmountFromValue(request.params[0]);
    } else if (request.params[0].get_real() == 50.0) {
        denomination = libzerocoin::ZQ_FIFTY;
        nAmount = AmountFromValue(request.params[0]);
    } else if (request.params[0].get_real() == 100.0) {
        denomination = libzerocoin::ZQ_ONE_HUNDRED;
        nAmount = AmountFromValue(request.params[0]);
    } else if (request.params[0].get_real() == 500.0) {
        denomination = libzerocoin::ZQ_FIVE_HUNDRED;
        nAmount = AmountFromValue(request.params[0]);
    } else if (request.params[0].get_real() == 1000.0) {
        denomination = libzerocoin::ZQ_ONE_THOUSAND;
        nAmount = AmountFromValue(request.params[0]);
    } else if (request.params[0].get_real() == 5000.0) {
        denomination = libzerocoin::ZQ_FIVE_THOUSAND;
        nAmount = AmountFromValue(request.params[0]);
    } else {
        throw runtime_error("mintzerocoin <amount>(1,5,10,50,100,500,1000,5000)\n");
    }
    LogPrintf("rpcWallet.mintzerocoin() denomination = %s, nAmount = %s \n", denomination, nAmount);


    // Set up the Zerocoin Params object
    libzerocoin::Params *zcParams = ZCParams;

    int mintVersion = 1;

    // The following constructor does all the work of minting a brand
    // new zerocoin. It stores all the private values inside the
    // PrivateCoin object. This includes the coin secrets, which must be
    // stored in a secure location (wallet) at the client.
    libzerocoin::PrivateCoin newCoin(zcParams, denomination, mintVersion);

    // Get a copy of the 'public' portion of the coin. You should
    // embed this into a Zerocoin 'MINT' transaction along with a series
    // of currency inputs totaling the assigned value of one zerocoin.
    libzerocoin::PublicCoin pubCoin = newCoin.getPublicCoin();

    // Validate
    if (pubCoin.validate()) {
        CScript scriptSerializedCoin =
                CScript() << OP_ZEROCOINMINT << pubCoin.getValue().getvch().size() << pubCoin.getValue().getvch();

        if (pwalletMain->IsLocked())
            throw JSONRPCError(RPC_WALLET_UNLOCK_NEEDED, "Error: Please enter the wallet passphrase with walletpassphrase first.");

        // Wallet comments
        CWalletTx wtx;

        string strError = pwalletMain->MintZerocoin(scriptSerializedCoin, nAmount, wtx);

        if (strError != "")
            throw JSONRPCError(RPC_WALLET_ERROR, strError);

        CWalletDB walletdb(pwalletMain->GetDBHandle());
        const unsigned char *ecdsaSecretKey = newCoin.getEcdsaSeckey();
        CZerocoinEntry zerocoinTx;
        zerocoinTx.IsUsed = false;
        zerocoinTx.denomination = denomination;
        zerocoinTx.value = pubCoin.getValue();
        zerocoinTx.randomness = newCoin.getRandomness();
        zerocoinTx.serialNumber = newCoin.getSerialNumber();
        zerocoinTx.ecdsaSecretKey = std::vector<unsigned char>(ecdsaSecretKey, ecdsaSecretKey+32);
        LogPrintf("CreateZerocoinMintModel() -> NotifyZerocoinChanged\n");
        LogPrintf("pubcoin=%s, isUsed=%s\n", zerocoinTx.value.GetHex(), zerocoinTx.IsUsed);
        LogPrintf("randomness=%s, serialNumber=%s\n", zerocoinTx.randomness.ToString(), zerocoinTx.serialNumber.ToString());
        pwalletMain->NotifyZerocoinChanged(pwalletMain, zerocoinTx.value.GetHex(), zerocoinTx.denomination, zerocoinTx.IsUsed ? "Used" : "New", CT_NEW);
        if (!walletdb.WriteZerocoinEntry(zerocoinTx))
            return false;
    } else {
        return "";
    }

    return "Sucessfully ghosted " + std::to_string(nAmount/COIN) +  " NIX";
}

UniValue spendzerocoin(const JSONRPCRequest& request) {

    CWallet * const pwalletMain = GetWalletForJSONRPCRequest(request);

    if (request.fHelp || request.params.size() > 2)
        throw runtime_error(
                "spendzerocoin <amount>(1,5,10,50,100,500,1000,5000) <spendtoaddress>(optional) \n"
                + HelpRequiringPassphrase(pwalletMain));


    int64_t nAmount = 0;
    libzerocoin::CoinDenomination denomination;
    // Amount
    if (request.params[0].get_real() == 1.0) {
        denomination = libzerocoin::ZQ_ONE;
        nAmount = AmountFromValue(request.params[0]);
    } else if (request.params[0].get_real() == 5.0) {
        denomination = libzerocoin::ZQ_FIVE;
        nAmount = AmountFromValue(request.params[0]);
    } else if (request.params[0].get_real() == 10.0) {
        denomination = libzerocoin::ZQ_TEN;
        nAmount = AmountFromValue(request.params[0]);
    } else if (request.params[0].get_real() == 50.0) {
        denomination = libzerocoin::ZQ_FIFTY;
        nAmount = AmountFromValue(request.params[0]);
    } else if (request.params[0].get_real() == 100.0) {
        denomination = libzerocoin::ZQ_ONE_HUNDRED;
        nAmount = AmountFromValue(request.params[0]);
    } else if (request.params[0].get_real() == 500.0) {
        denomination = libzerocoin::ZQ_FIVE_HUNDRED;
        nAmount = AmountFromValue(request.params[0]);
    } else if (request.params[0].get_real() == 1000.0) {
        denomination = libzerocoin::ZQ_ONE_THOUSAND;
        nAmount = AmountFromValue(request.params[0]);
    } else if (request.params[0].get_real() == 5000.0) {
        denomination = libzerocoin::ZQ_FIVE_THOUSAND;
        nAmount = AmountFromValue(request.params[0]);
    } else {
        throw runtime_error("spendzerocoin <amount>(1,5,10,50,100,500,1000,5000) <spendtoaddress>(optional)\n");
    }

    CBitcoinAddress address;
    string toKey = "";
    if (request.params.size() > 1){
        // Address
        toKey = request.params[1].get_str();
        address = CBitcoinAddress(request.params[1].get_str());

        if(!IsStealthAddress(toKey))
            if (!address.IsValid())
                throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "rpcwallet spendzerocoin(): Invalid toKey address");
    }

    if (pwalletMain->IsLocked())
        throw JSONRPCError(RPC_WALLET_UNLOCK_NEEDED,
                           "Error: Please enter the wallet passphrase with walletpassphrase first.");

    // Wallet comments
    CWalletTx wtx;
    CBigNum coinSerial;
    uint256 txHash;
    CBigNum zcSelectedValue;
    bool zcSelectedIsUsed;

    string strError = pwalletMain->SpendZerocoin(toKey, nAmount, denomination, wtx, coinSerial, txHash, zcSelectedValue,
                                                 zcSelectedIsUsed);

    if (strError != "")
        throw JSONRPCError(RPC_WALLET_ERROR, strError);

    return wtx.GetHash().GetHex();

}

UniValue resetmintzerocoin(const JSONRPCRequest& request) {

    CWallet * const pwalletMain = GetWalletForJSONRPCRequest(request);

    if (request.fHelp || request.params.size() != 0)
        throw runtime_error(
                "resetmintzerocoin"
                + HelpRequiringPassphrase(pwalletMain));


    list <CZerocoinEntry> listPubcoin;
    CWalletDB walletdb(pwalletMain->GetDBHandle());
    walletdb.ListPubCoin(listPubcoin);

    for(const CZerocoinEntry &zerocoinItem: listPubcoin){
        if (zerocoinItem.randomness != 0 && zerocoinItem.serialNumber != 0) {
            CZerocoinEntry zerocoinTx;
            zerocoinTx.IsUsed = false;
            zerocoinTx.denomination = zerocoinItem.denomination;
            zerocoinTx.value = zerocoinItem.value;
            zerocoinTx.serialNumber = zerocoinItem.serialNumber;
            zerocoinTx.nHeight = -1;
            zerocoinTx.randomness = zerocoinItem.randomness;
            walletdb.WriteZerocoinEntry(zerocoinTx);
        }
    }

    return NullUniValue;
}

UniValue listmintzerocoins(const JSONRPCRequest& request) {
    if (request.fHelp || request.params.size() > 1)
        throw runtime_error(
                "listmintzerocoins <all>(false/true)\n"
                        "\nArguments:\n"
                        "1. <all> (boolean, optional) false (default) to return own mintzerocoins. true to return every mintzerocoins.\n"
                        "\nResults are an array of Objects, each of which has:\n"
                        "{id, IsUsed, denomination, value, serialNumber, nHeight, randomness}");

    bool fAllStatus = false;
    if (request.params.size() > 0) {
        fAllStatus = request.params[0].get_bool();
    }

    CWallet * const pwalletMain = GetWalletForJSONRPCRequest(request);

    list <CZerocoinEntry> listPubcoin;
    CWalletDB walletdb(pwalletMain->GetDBHandle());
    walletdb.ListPubCoin(listPubcoin);
    UniValue results(UniValue::VARR);

    for(const CZerocoinEntry &zerocoinItem: listPubcoin) {
        if (fAllStatus || zerocoinItem.IsUsed || (zerocoinItem.randomness != 0 && zerocoinItem.serialNumber != 0)) {
            UniValue entry(UniValue::VOBJ);
            entry.push_back(Pair("id", zerocoinItem.id));
            entry.push_back(Pair("IsUsed", zerocoinItem.IsUsed));
            entry.push_back(Pair("denomination", zerocoinItem.denomination));
            entry.push_back(Pair("value", zerocoinItem.value.GetHex()));
            entry.push_back(Pair("serialNumber", zerocoinItem.serialNumber.GetHex()));
            entry.push_back(Pair("nHeight", zerocoinItem.nHeight));
            entry.push_back(Pair("randomness", zerocoinItem.randomness.GetHex()));
            entry.push_back(Pair("seckey", CBigNum(zerocoinItem.ecdsaSecretKey).GetHex()));
            results.push_back(entry);
        }
    }

    return results;
}

UniValue listpubcoins(const JSONRPCRequest& request) {
    if (request.fHelp || request.params.size() > 1)
        throw runtime_error(
                "listpubcoin <all>(1/5/10/50/100/500/1000/5000)\n"
                        "\nArguments:\n"
                        "1. <all> (int, optional) 1,5,10,50,100,500,1000,5000 (default) to return all pubcoin with denomination. empty to return all pubcoin.\n"
                        "\nResults are an array of Objects, each of which has:\n"
                        "{id, IsUsed, denomination, value, serialNumber, nHeight, randomness}");

    int denomination = -1;
    if (request.params.size() > 0) {
        denomination = request.params[0].get_int();
    }

    CWallet * const pwalletMain = GetWalletForJSONRPCRequest(request);

    list <CZerocoinEntry> listPubcoin;
    CWalletDB walletdb(pwalletMain->GetDBHandle());
    walletdb.ListPubCoin(listPubcoin);
    UniValue results(UniValue::VARR);
    listPubcoin.sort(CompID);

    for(const CZerocoinEntry &zerocoinItem: listPubcoin) {
        if (zerocoinItem.id > 0 && (denomination < 0 || zerocoinItem.denomination == denomination)) {
            UniValue entry(UniValue::VOBJ);
            entry.push_back(Pair("id", zerocoinItem.id));
            entry.push_back(Pair("IsUsed", zerocoinItem.IsUsed));
            entry.push_back(Pair("denomination", zerocoinItem.denomination));
            entry.push_back(Pair("value", zerocoinItem.value.GetHex()));
            entry.push_back(Pair("serialNumber", zerocoinItem.serialNumber.GetHex()));
            entry.push_back(Pair("nHeight", zerocoinItem.nHeight));
            entry.push_back(Pair("randomness", zerocoinItem.randomness.GetHex()));
            results.push_back(entry);
        }
    }

    return results;
}

UniValue setmintzerocoinstatus(const JSONRPCRequest& request) {
    if (request.fHelp || request.params.size() != 2)
        throw runtime_error(
                "setmintzerocoinstatus \"coinserial\" <isused>(true/false)\n"
                        "Set mintzerocoin IsUsed status to True or False\n"
                        "Results are an array of one or no Objects, each of which has:\n"
                        "{id, IsUsed, denomination, value, serialNumber, nHeight, randomness}");

    CBigNum coinSerial;
    coinSerial.SetHex(request.params[0].get_str());

    bool fStatus = true;
    fStatus = request.params[1].get_bool();

    CWallet * const pwalletMain = GetWalletForJSONRPCRequest(request);

    list <CZerocoinEntry> listPubcoin;
    CWalletDB walletdb(pwalletMain->GetDBHandle());
    walletdb.ListPubCoin(listPubcoin);

    UniValue results(UniValue::VARR);

    for(const CZerocoinEntry &zerocoinItem: listPubcoin) {
        if (zerocoinItem.serialNumber != 0) {
            LogPrintf("zerocoinItem.serialNumber = %s\n", zerocoinItem.serialNumber.GetHex());
            if (zerocoinItem.serialNumber == coinSerial) {
                LogPrintf("setmintzerocoinstatus Found!\n");
                CZerocoinEntry zerocoinTx;
                zerocoinTx.id = zerocoinItem.id;
                zerocoinTx.IsUsed = fStatus;
                zerocoinTx.denomination = zerocoinItem.denomination;
                zerocoinTx.value = zerocoinItem.value;
                zerocoinTx.serialNumber = zerocoinItem.serialNumber;
                zerocoinTx.nHeight = zerocoinItem.nHeight;
                zerocoinTx.randomness = zerocoinItem.randomness;
                const std::string& isUsedDenomStr = zerocoinTx.IsUsed
                        ? "Used (" + std::to_string(zerocoinTx.denomination) + " mint)"
                        : "New (" + std::to_string(zerocoinTx.denomination) + " mint)";
                pwalletMain->NotifyZerocoinChanged(pwalletMain, zerocoinTx.value.GetHex(), zerocoinTx.denomination, isUsedDenomStr, CT_UPDATED);
                walletdb.WriteZerocoinEntry(zerocoinTx);

                UniValue entry(UniValue::VOBJ);
                entry.push_back(Pair("id", zerocoinTx.id));
                entry.push_back(Pair("IsUsed", zerocoinTx.IsUsed));
                entry.push_back(Pair("denomination", zerocoinTx.denomination));
                entry.push_back(Pair("value", zerocoinTx.value.GetHex()));
                entry.push_back(Pair("serialNumber", zerocoinTx.serialNumber.GetHex()));
                entry.push_back(Pair("nHeight", zerocoinTx.nHeight));
                entry.push_back(Pair("randomness", zerocoinTx.randomness.GetHex()));
                results.push_back(entry);
                break;
            }
        }
    }

    return results;
}

//TOR/I2P Config
UniValue enableTor(const JSONRPCRequest& request){

    if (request.fHelp || request.params.size() > 1)
        throw runtime_error(
                "enabletor <enable>(false/true)\n"
                        "To enable obfuscation, set enabletor to \"true\"\n"
                        "Please restart the NIX daemon to update your changes");

    bool fStatus = true;
    std::string sfStatus = request.params[0].get_str();
    if(sfStatus == "true")
        fStatus = true;
    else if(sfStatus == "false")
        fStatus = false;
    else
        throw runtime_error(
                "enabletor <enable>(false/true)\n"
                        "To enable obfuscation, set enabletor to \"true\"\n"
                        "Please restart the NIX daemon to update your changes");

    std::string result = "Error with enabletor feature\n";
    boost::filesystem::path pathTorSetting = GetDataDir()/"nixtorsetting.dat";
    if(fStatus){
        if (WriteBinaryFileTor(pathTorSetting.string().c_str(), "enabled")) {
            result = ("Please restart the NIX Core wallet to route your connection to obfuscate your IP address. \nSyncing your wallet might be slower.");
        }else{
            result = ("Obfuscation cannot enable");
        }
    }else{
        if (WriteBinaryFileTor(pathTorSetting.string().c_str(), "disabled")) {
            result = ("Please restart the NIX Core wallet to disable IP obfuscation.");
        } else {
            result = ("Obfuscation cannot disable");
        }
    }
    return result;
}

UniValue torStatus(const JSONRPCRequest& request){

    if (request.fHelp || request.params.size() > 1)
        throw runtime_error(
                "torstatus\n"
                        "Returns the status of tor obfuscation on your NIX daemon");

    boost::filesystem::path pathTorSetting = GetDataDir()/"nixtorsetting.dat";
    std::string result = "Error with torstatus feature\n";
    // read config
    std::pair<bool,std::string> torEnabled = ReadBinaryFileTor(pathTorSetting.string().c_str());
    if(torEnabled.first){
        if(torEnabled.second == "enabled"){
            result =  "Obfuscation Enabled";
        }else{
            result = "Obfuscation Disabled";
        }
    }
    return result;
}

#include <script/ismine.h>
UniValue getalladdresses(const JSONRPCRequest &request)
{
    CWallet *pwallet = GetWalletForJSONRPCRequest(request);
    if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() > 0)
        throw std::runtime_error(
            "getalladdresses \n"
            "Get all send addresses.\n");

    UniValue result(UniValue::VOBJ);

    UniValue send(UniValue::VOBJ);
    UniValue receive(UniValue::VOBJ);

    for(std::pair<CTxDestination, CAddressBookData> add :pwallet->mapAddressBook){
        CBitcoinAddress walletAddress(add.first);

        if(::IsMine(*pwallet,add.first))
            receive.pushKV(walletAddress.ToString(), add.second.name);
        else
            send.pushKV(walletAddress.ToString(), add.second.name);
    }
    result.pushKV("receive", receive);
    result.pushKV("send", send);

    return result;
}

UniValue manageaddressbook(const JSONRPCRequest &request)
{
    CWallet *pwallet = GetWalletForJSONRPCRequest(request);
    if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() < 2 || request.params.size() > 4)
        throw std::runtime_error(
            "manageaddressbook \"action\" \"address\" ( \"label\" \"purpose\" )\n"
            "Manage the address book."
            "\nArguments:\n"
            "1. \"action\"      (string, required) 'add/edit/del/info/newsend' The action to take.\n"
            "2. \"address\"     (string, required) The address to affect.\n"
            "3. \"label\"       (string, optional) Optional label.\n"
            "4. \"purpose\"     (string, optional) Optional purpose label.\n");

    ObserveSafeMode();

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pwallet->BlockUntilSyncedToCurrentChain();

    std::string sAction = request.params[0].get_str();
    std::string sAddress = request.params[1].get_str();
    std::string sLabel, sPurpose;

    bool fHavePurpose = false;
    if (request.params.size() > 2)
        sLabel = request.params[2].get_str();
    if (request.params.size() > 3)
    {
        sPurpose = request.params[3].get_str();
        fHavePurpose = true;
    };

    CBitcoinAddress address(sAddress);

    if (!address.IsValid())
        throw JSONRPCError(RPC_INVALID_PARAMETER, _("Invalid NIX address."));

    CTxDestination dest = address.Get();

    std::map<CTxDestination, CAddressBookData>::iterator mabi;
    mabi = pwallet->mapAddressBook.find(dest);

    UniValue objDestData(UniValue::VOBJ);

    if (sAction == "add")
    {
        if (mabi != pwallet->mapAddressBook.end())
            throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf(_("Address '%s' is recorded in the address book."), sAddress));

        if (!pwallet->SetAddressBook(dest, sLabel, sPurpose))
            throw JSONRPCError(RPC_WALLET_ERROR, "SetAddressBook failed.");
    } else
    if (sAction == "edit")
    {
        if (request.params.size() < 3)
            throw JSONRPCError(RPC_INVALID_PARAMETER, _("Need a parameter to change."));
        if (mabi == pwallet->mapAddressBook.end())
            throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf(_("Address '%s' is not in the address book."), sAddress));

        if (!pwallet->SetAddressBook(dest, sLabel,
            fHavePurpose ? sPurpose : mabi->second.purpose))
            throw JSONRPCError(RPC_WALLET_ERROR, "SetAddressBook failed.");

        sLabel = mabi->second.name;
        sPurpose = mabi->second.purpose;

        for (const auto &pair : mabi->second.destdata)
            objDestData.pushKV(pair.first, pair.second);

    } else
    if (sAction == "del")
    {
        if (mabi == pwallet->mapAddressBook.end())
            throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf(_("Address '%s' is not in the address book."), sAddress));
        sLabel = mabi->second.name;
        sPurpose = mabi->second.purpose;

        if (!pwallet->DelAddressBook(dest))
            throw JSONRPCError(RPC_WALLET_ERROR, "DelAddressBook failed.");
    } else
    if (sAction == "info")
    {
        if (mabi == pwallet->mapAddressBook.end())
            throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf(_("Address '%s' is not in the address book."), sAddress));

        UniValue result(UniValue::VOBJ);

        result.pushKV("action", sAction);
        result.pushKV("address", sAddress);

        result.pushKV("label", mabi->second.name);
        result.pushKV("purpose", mabi->second.purpose);

        if (mabi->second.nOwned == 0)
            mabi->second.nOwned = ::IsMine(*pwallet, mabi->first) ? 1 : 2;

        result.pushKV("owned", mabi->second.nOwned == 1 ? "true" : "false");

        if (mabi->second.vPath.size() > 1)
        {
            std::string sPath;
            if (0 == PathToString(mabi->second.vPath, sPath, '\'', 1))
                result.pushKV("path", sPath);
        };

        for (const auto &pair : mabi->second.destdata)
            objDestData.pushKV(pair.first, pair.second);
        if (objDestData.size() > 0)
            result.pushKV("destdata", objDestData);

        result.pushKV("result", "success");

        return result;
    } else
    {
        throw JSONRPCError(RPC_INVALID_PARAMETER, _("Unknown action, must be one of 'add/edit/del'."));
    };

    UniValue result(UniValue::VOBJ);

    result.pushKV("action", sAction);
    result.pushKV("address", sAddress);

    if (sLabel.size() > 0)
        result.pushKV("label", sLabel);
    if (sPurpose.size() > 0)
        result.pushKV("purpose", sPurpose);
    if (objDestData.size() > 0)
        result.pushKV("destdata", objDestData);

    result.pushKV("result", "success");

    return result;
}

/*********************/
/* Staking Protocol */

UniValue getstakinginfo(const JSONRPCRequest &request)
{
    CWallet *pwallet = GetWalletForJSONRPCRequest(request);
    if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() > 1)
        throw std::runtime_error(
            "getstakinginfo\n"
            "Returns an object containing staking-related information."
            "\nResult:\n"
            "{\n"
            "  \"enabled\": true|false,         (boolean) if staking is enabled or not on this wallet\n"
            "  \"staking\": true|false,         (boolean) if this wallet is staking or not\n"
            "  \"errors\": \"...\"              (string) any error messages\n"
            "  \"percentyearreward\": xxxxxxx,  (numeric) current stake reward percentage\n"
            "  \"moneysupply\": xxxxxxx,        (numeric) the total amount of NIX in the network\n"
            "  \"reserve\": xxxxxxx,            (numeric) the total amount of NIX in the network\n"
            "  \"walletdonationpercent\": xxxxxxx,\n    (numeric) user set percentage of the block reward ceded to development\n"
            "  \"currentblocksize\": nnn,       (numeric) the last block size in bytes\n"
            "  \"currentblockweight\": nnn,     (numeric) the last block weight\n"
            "  \"currentblocktx\": nnn,         (numeric) the number of transactions in the last block\n"
            "  \"pooledtx\": n                  (numeric) the number of transactions in the mempool\n"
            "  \"difficulty\": xxx.xxxxx        (numeric) the current difficulty\n"
            "  \"lastsearchtime\": xxxxxxx      (numeric) the last time this wallet searched for a coinstake\n"
            "  \"weight\": xxxxxxx              (numeric) the current stake weight of this wallet\n"
            "  \"netstakeweight\": xxxxxxx      (numeric) the current stake weight of the network\n"
            "  \"expectedtime\": xxxxxxx        (numeric) estimated time for next stake\n"
            "}\n"
            "\nExamples:\n"
            + HelpExampleCli("getstakinginfo", "")
            + HelpExampleRpc("getstakinginfo", ""));

    ObserveSafeMode();

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pwallet->BlockUntilSyncedToCurrentChain();

    UniValue obj(UniValue::VOBJ);

    int64_t nTipTime;
    float rCoinYearReward;
    CAmount nMoneySupply;
    {
        LOCK(cs_main);
        nTipTime = chainActive.Tip()->nTime;
        rCoinYearReward = Params().GetCoinYearReward(nTipTime) / CENT;
        nMoneySupply = chainActive.Tip()->nMoneySupply;
    }

    uint64_t nWeight = pwallet->GetStakeWeight();

    uint64_t nNetworkWeight = GetPoSKernelPS();

    bool fStaking = nWeight && fIsStaking;
    uint64_t nExpectedTime = fStaking ? (Params().GetTargetSpacing() * nNetworkWeight / nWeight) : 0;

    obj.pushKV("enabled", gArgs.GetBoolArg("-staking", true));
    obj.pushKV("staking", fStaking && pwallet->nIsStaking == CWallet::IS_STAKING);
    switch (pwallet->nIsStaking)
    {
        case CWallet::NOT_STAKING_BALANCE:
            obj.pushKV("cause", "low_balance");
            break;
        case CWallet::NOT_STAKING_DEPTH:
            obj.pushKV("cause", "low_depth");
            break;
        case CWallet::NOT_STAKING_LOCKED:
            obj.pushKV("cause", "locked");
            break;
        case CWallet::NOT_STAKING_LIMITED:
            obj.pushKV("cause", "limited");
            break;
        case CWallet::NOT_STAKING_NOT_UNLOCKED_FOR_STAKING_ONLY:
            obj.pushKV("cause", "not unlocked for staking");
            break;
        default:
            break;
    };

    obj.pushKV("errors", GetWarnings("statusbar"));

    obj.pushKV("percentyearreward", rCoinYearReward);
    obj.pushKV("moneysupply", ValueFromAmount(nMoneySupply));

    if (pwallet->nReserveBalance > 0)
        obj.pushKV("reserve", ValueFromAmount(pwallet->nReserveBalance));

    if (pwallet->nWalletDonationPercent > 0)
        obj.pushKV("walletdonationpercent", pwallet->nWalletDonationPercent);
    if (pwallet->nWalletDonationAddress != "")
        obj.pushKV("walletdonationaddress", pwallet->nWalletDonationAddress);

    obj.pushKV("currentblocksize", (uint64_t)nLastBlockSize);
    obj.pushKV("currentblocktx", (uint64_t)nLastBlockTx);
    obj.pushKV("pooledtx", (uint64_t)mempool.size());

    obj.pushKV("difficulty", GetDifficulty());
    obj.pushKV("lastsearchtime", (uint64_t)pwallet->nLastCoinStakeSearchTime);

    obj.pushKV("weight", (uint64_t)nWeight);
    obj.pushKV("netstakeweight", (uint64_t)nNetworkWeight);

    obj.pushKV("expectedtime", nExpectedTime);

    LOCK(cs_main);

    if(request.params.size() == 1){
        CAmount totalSupply = 0;
        int amountofOuts = 0;

        // manually verify all output amounts
        for(auto it = 0; it < chainActive.Height(); it++){
            CBlock block;
            CBlockIndex *pindex = chainActive[it];
            if (ReadBlockFromDisk(block, pindex, Params().GetConsensus())){
                for(auto ctx: block.vtx){
                    for(int ss = 0; ss < ctx->vout.size(); ss++){
                        if(ctx->vout[ss].scriptPubKey.IsZerocoinMint())
                            continue;
                        COutPoint out(ctx->GetHash(), ss);
                        if(pcoinsTip->HaveCoin(out)){
                            amountofOuts++;
                            totalSupply += ctx->vout[ss].nValue;
                        }
                    }
                }
            }
            else
                return "ReadBlockFromDisk failed!";
        }

        obj.pushKV("totalpublicsupply", ValueFromAmount(totalSupply));
        obj.pushKV("outputs", amountofOuts);
    }

    return obj;
}

UniValue getcoldstakinginfo(const JSONRPCRequest &request)
{
    CWallet *pwallet = GetWalletForJSONRPCRequest(request);
    if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() != 0)
        throw std::runtime_error(
            "getcoldstakinginfo\n"
            "Returns an object containing coldstaking related information."
            "\nResult:\n"
            "{\n"
            "  \"enabled\": true|false,             (boolean) If a valid coldstakingaddress is loaded or not on this wallet.\n"
            "  \"coldstaking_address\"              (string) The address of the current coldstakingaddress.\n"
            "  \"coin_in_stakeable_script\"         (numeric) Current amount of coin in scripts stakeable by this wallet.\n"
            "  \"coin_in_coldstakeable_script\"     (numeric) Current amount of coin in scripts stakeable by the wallet with the coldstakingaddress.\n"
            "  \"percent_in_coldstakeable_script\"  (numeric) Percentage of coin in coldstakeable scripts.\n"
            "  \"currently_staking\"                (numeric) Amount of coin estimated to be currently staking by this wallet.\n"
            "}\n"
            "\nExamples:\n"
            + HelpExampleCli("getcoldstakinginfo", "")
            + HelpExampleRpc("getcoldstakinginfo", ""));

    ObserveSafeMode();

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pwallet->BlockUntilSyncedToCurrentChain();

    UniValue obj(UniValue::VOBJ);

    LOCK2(cs_main, pwallet->cs_wallet);
    std::vector<COutput> vecOutputs;

    bool include_unsafe = false;
    //bool fIncludeImmature = true;
    CAmount nMinimumAmount = 0;
    CAmount nMaximumAmount = MAX_MONEY;
    CAmount nMinimumSumAmount = MAX_MONEY;
    uint64_t nMaximumCount = 0;
    int nMinDepth = 0;
    int nMaxDepth = 0x7FFFFFFF;

    int nHeight = chainActive.Tip()->nHeight;

    int nRequiredDepth = nHeight >= Params().GetConsensus().nCoinMaturityReductionHeight ?
                COINBASE_MATURITY_V2 : COINBASE_MATURITY;

    bool fTestNet = (Params().NetworkIDString() == CBaseChainParams::TESTNET);
    if(fTestNet)
        nRequiredDepth = COINBASE_MATURITY_TESTNET;

    pwallet->AvailableCoins(vecOutputs, include_unsafe, nullptr, nMinimumAmount, nMaximumAmount, nMinimumSumAmount, nMaximumCount, nMinDepth, nMaxDepth, AvailableCoinsType::ALL_COINS, true);

    CAmount nStakeable = 0;
    CAmount nColdStakeable = 0;
    CAmount nWalletStaking = 0;

    CScriptID keyID;
    WitnessV0KeyHash witness_ID;
    for (const auto &out : vecOutputs)
    {
        const CScript scriptPubKey = out.tx->tx->vout[out.i].scriptPubKey;
        CAmount nValue = out.tx->tx->vout[out.i].nValue;
        LogPrintf("\n IsPayToScriptHash_CS size: %d", scriptPubKey.size());
        if (scriptPubKey.IsPayToScriptHash())
        {
            if (!out.fSpendable)
                continue;
            nStakeable += nValue;
        } else
        if (scriptPubKey.IsPayToScriptHash_CS())
        {
            // Show output on both the spending and staking wallets
            if (!out.fSpendable)
            {
                if (!ExtractStakingKeyID(scriptPubKey, keyID, witness_ID))
                    continue;
                if(!pwallet->HaveCScript(keyID))
                    continue;
            }
            nColdStakeable += nValue;
        } else
        {
            continue;
        }

        if (out.nDepth < nRequiredDepth)
            continue;

        if (!ExtractStakingKeyID(scriptPubKey, keyID, witness_ID))
            continue;

        if(pwallet->HaveCScript(keyID))
            nWalletStaking += nValue;
    }


    CBitcoinAddress addrColdStaking;
    std::string sAddress = gArgs.GetArg("-coldstakeaddress", "");
    if (sAddress != "")
    {
        addrColdStaking.SetString(sAddress);
        if (addrColdStaking.IsValid()){
            obj.pushKV("enabled", true);
            obj.pushKV("coldstaking_address", addrColdStaking.ToString());
        }
        else
            obj.pushKV("enabled", false);
    }
    else{
        obj.pushKV("enabled", false);
        obj.pushKV("coldstaking_address", "");
    }


    obj.pushKV("coin_in_stakeable_script", ValueFromAmount(nStakeable));
    obj.pushKV("coin_in_coldstakeable_script", ValueFromAmount(nColdStakeable));
    CAmount nTotal = nColdStakeable + nStakeable;
    obj.pushKV("percent_in_coldstakeable_script",
        UniValue(UniValue::VNUM, strprintf("%.2f", nTotal == 0 ? 0.0 : (nColdStakeable * 10000 / nTotal) / 100.0)));
    obj.pushKV("currently_staking", ValueFromAmount(nWalletStaking));

    return obj;
}

UniValue reservebalance(const JSONRPCRequest &request)
{
    // Reserve balance from being staked for network protection

    CWallet *pwallet = GetWalletForJSONRPCRequest(request);
    if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() > 2)
        throw std::runtime_error(
            "reservebalance reserve ( amount )\n"
            "reserve is true or false to turn balance reserve on or off.\n"
            "amount is a real and rounded to cent.\n"
            "Set reserve amount not participating in network protection.\n"
            "If no parameters provided current setting is printed.\n"
            "Wallet must be unlocked to modify.\n");

    if (request.params.size() > 0)
    {
        EnsureWalletIsUnlocked(pwallet);

        bool fReserve = request.params[0].get_bool();
        if (fReserve)
        {
            if (request.params.size() == 1)
                throw JSONRPCError(RPC_INVALID_PARAMETER, "must provide amount to reserve balance.");
            int64_t nAmount = AmountFromValue(request.params[1]);
            nAmount = (nAmount / CENT) * CENT;  // round to cent
            if (nAmount < 0)
                throw JSONRPCError(RPC_INVALID_PARAMETER, "amount cannot be negative.");
            pwallet->SetReserveBalance(nAmount);
        } else
        {
            if (request.params.size() > 1)
                throw JSONRPCError(RPC_INVALID_PARAMETER, "cannot specify amount to turn off reserve.");
            pwallet->SetReserveBalance(0);
        };
        WakeThreadStakeMiner(pwallet);
    };

    UniValue result(UniValue::VOBJ);
    result.pushKV("reserve", (pwallet->nReserveBalance > 0));
    result.pushKV("amount", ValueFromAmount(pwallet->nReserveBalance));
    return result;
}

UniValue refillghostkeys(const JSONRPCRequest& request)
{

    CWallet *pwalletMain = GetWalletForJSONRPCRequest(request);

    if (request.fHelp || request.params.size() > 1)
        throw runtime_error("refillghostkeys <amount>(default=100)\n" + HelpRequiringPassphrase(pwalletMain));

    libzerocoin::CoinDenomination denomination;
    libzerocoin::Params *zcParams = ZCParams;

    int mintVersion = 1;
    denomination = libzerocoin::ZQ_ONE;

    vector<std::string> ghostKey;

    if (pwalletMain->IsLocked()) {
        throw JSONRPCError(RPC_WALLET_UNLOCK_NEEDED, "Error: Please enter the wallet passphrase with walletpassphrase first.");
    }

    list <CZerocoinEntry> listUnloadedPubcoin;
    CWalletDB walletdb(pwalletMain->GetDBHandle());
    walletdb.ListUnloadedPubCoin(listUnloadedPubcoin);

    int ideal = 0;
    if(request.params.size() > 0)
        ideal = request.params[0].get_int();
    else
        ideal = 101;

    //refill keys to 100 in wallet
    for(int i = listUnloadedPubcoin.size(); i < ideal; i++){
        libzerocoin::PrivateCoin newCoinTemp(zcParams, denomination, mintVersion);
        if(newCoinTemp.getPublicCoin().validate()){
            const unsigned char *ecdsaSecretKey = newCoinTemp.getEcdsaSeckey();
            CZerocoinEntry zerocoinTx;
            zerocoinTx.IsUsed = false;
            zerocoinTx.denomination = libzerocoin::ZQ_ERROR;
            zerocoinTx.value = newCoinTemp.getPublicCoin().getValue();
            zerocoinTx.randomness = newCoinTemp.getRandomness();
            zerocoinTx.serialNumber = newCoinTemp.getSerialNumber();
            zerocoinTx.ecdsaSecretKey = std::vector<unsigned char>(ecdsaSecretKey, ecdsaSecretKey+32);
            if (!walletdb.WriteUnloadedZCEntry(zerocoinTx))
                return "ghostkeys() Error: Only able to create " + std::to_string(i) + " keys";

            std::vector<unsigned char> commitmentKey = newCoinTemp.getPublicCoin().getValue().getvch();
            CommitmentKey pubCoin(commitmentKey);
            ghostKey.push_back(pubCoin.GetPubCoinDataBase58() + "-");
        }
        else
            i--;
    }

    string fullKey;
    for(string key: ghostKey)
        fullKey+=key;

    UniValue results(UniValue::VARR);
    results.push_back("Sucessfully created ghostkey amount: " + fullKey);
    return results;
}

UniValue listunloadedpubcoins(const JSONRPCRequest& request) {
    if (request.fHelp || request.params.size() > 1)
        throw runtime_error(
                "listunloadedpubcoins amount(default=all)\n"
                        "\nResults are an array of public ghost keys:\n");


    CWallet * const pwalletMain = GetWalletForJSONRPCRequest(request);

    list <CZerocoinEntry> listUnloadedPubcoin;
    CWalletDB walletdb(pwalletMain->GetDBHandle());
    walletdb.ListUnloadedPubCoin(listUnloadedPubcoin);
    UniValue results(UniValue::VARR);
    //listUnloadedPubcoin.sort(CompID);

    for(const CZerocoinEntry &zerocoinItem: listUnloadedPubcoin) {
        std::vector<unsigned char> commitmentKey = zerocoinItem.value.getvch();
        CommitmentKey pubCoin(commitmentKey);
        results.push_back(pubCoin.GetPubCoinDataBase58());
    }

    return results;
}

UniValue getpubcoinpack(const JSONRPCRequest& request) {

    if (request.fHelp || request.params.size() > 1)
        throw runtime_error(
                "getpubcoinpack amount(default=10)\n"
                        "\nResults a Commitment Key Pack\n");


    CWallet * const pwalletMain = GetWalletForJSONRPCRequest(request);

    /*
    if (pwalletMain->IsLocked()) {
        throw JSONRPCError(RPC_WALLET_UNLOCK_NEEDED, "Error: Please enter the wallet passphrase with walletpassphrase first.");
    }
    */

    list <CZerocoinEntry> listUnloadedPubcoin;
    CWalletDB walletdb(pwalletMain->GetDBHandle());
    walletdb.ListUnloadedPubCoin(listUnloadedPubcoin);
    UniValue results(UniValue::VARR);
    //listUnloadedPubcoin.sort(CompID);

    int keyAmount = 10;
    if(request.params.size() > 0)
        keyAmount = request.params[0].get_int();

    if(keyAmount > listUnloadedPubcoin.size())
        throw JSONRPCError(RPC_WALLET_ERROR,
                           "Error: Not enough Commitment Keys, please run refillghostkeys");

    std::vector< std::vector <unsigned char>> keyList = std::vector< std::vector <unsigned char>>();
    keyList.clear();
    for(const CZerocoinEntry &zerocoinItem: listUnloadedPubcoin) {
        if(keyAmount < 1)
            break;
        keyAmount--;
        std::vector<unsigned char> commitmentKey = zerocoinItem.value.getvch();
        keyList.push_back(commitmentKey);
    }

    CommitmentKeyPack pubCoinPack(keyList);

    results.push_back(pubCoinPack.GetPubCoinPackDataBase58());

    return results;
}

UniValue payunloadedpubcoins(const JSONRPCRequest& request) {

    if (request.fHelp || request.params.size() > 2)
        throw runtime_error(
                "payunloadedpubcoins\n"
                        "\nArguments:\n"
                        "\nAmount to pay\n"
                        "\nGhost key string:\n");


    CWallet * const pwalletMain = GetWalletForJSONRPCRequest(request);

    int64_t nAmount = request.params[0].get_int64();

    if (pwalletMain->IsLocked())
        throw JSONRPCError(RPC_WALLET_UNLOCK_NEEDED,
                           "Error: Please enter the wallet passphrase with walletpassphrase first.");


    //split key into convertable format
    std::string keyPackString = request.params[1].get_str();
    CommitmentKeyPack keyPack(keyPackString);

    std::string strError;

    if(keyPack.IsValidPack())
        strError = pwalletMain->GhostModeSpendTrigger(std::to_string(nAmount), "", keyPack.GetPubCoinPackScript());
    else
        return "Not Valid Pack";

    UniValue results(UniValue::VARR);
    results.push_back(strError);
    return results;
}


UniValue resetzerocoinamounts(const JSONRPCRequest& request)
{

    if (request.fHelp || request.params.size() > 0)
        throw runtime_error(
                "resetzerocoinamounts\n"
                        "\Erases unconfirmed zerocoins\n");

    CWallet *pwalletMain = GetWalletForJSONRPCRequest(request);


    list <CZerocoinEntry> listPubcoin;
    CWalletDB walletdb(pwalletMain->GetDBHandle());
    walletdb.ListPubCoin(listPubcoin);

    UniValue results(UniValue::VARR);

    for(CZerocoinEntry &zcEntry: listPubcoin){
        if (!walletdb.EraseZerocoinEntry(zcEntry)){
            results.push_back("Unable to erase zerocoins");
            return results;
        }
    }

    results.push_back("Sucessfully erased all zerocoins");
    return results;
}

UniValue resetzerocoinunconfirmed(const JSONRPCRequest& request)
{

    if (request.fHelp || request.params.size() > 0)
        throw runtime_error(
                "resetzerocoinunconfirmed\n"
                        "\Erases unconfirmed zerocoins\n");

    CWallet *pwalletMain = GetWalletForJSONRPCRequest(request);

    {
        LOCK(pwalletMain->cs_wallet);
        list <CZerocoinEntry> listPubCoin = list<CZerocoinEntry>();
        CWalletDB walletdb(pwalletMain->GetDBHandle());
        walletdb.ListPubCoin(listPubCoin);
        for (map<uint256, CWalletTx>::const_iterator it = pwalletMain->mapWallet.begin(); it != pwalletMain->mapWallet.end(); ++it) {
            const CWalletTx *pcoin = &(*it).second;
            //            LogPrintf("pcoin=%s\n", pcoin->GetHash().ToString());
            if (!CheckFinalTx(*pcoin->tx,0)) {
                //LogPrintf("!CheckFinalTx(*pcoin)=%s\n", !CheckFinalTx(*pcoin->tx,0));
                continue;
            }

            if ((pcoin->IsCoinBase() || pcoin->IsCoinStake()) && pcoin->GetBlocksToMaturity() > 0) {
                //LogPrintf("Not trusted\n");
                continue;
            }

            int nDepth = pcoin->GetDepthInMainChain();
            if (nDepth < 0) {
                //LogPrintf("nDepth=%s\n", nDepth);
                continue;
            }

            for (unsigned int i = 0; i < pcoin->tx->vout.size(); i++) {
                if (pcoin->tx->vout[i].scriptPubKey.IsZerocoinMint()) {
                    CTxOut txout = pcoin->tx->vout[i];
                    vector<unsigned char> vchZeroMint;
                    vchZeroMint.insert(vchZeroMint.end(), txout.scriptPubKey.begin() + 6,
                                       txout.scriptPubKey.begin() + txout.scriptPubKey.size());

                    CBigNum pubCoin;
                    pubCoin.setvch(vchZeroMint);
                    //LogPrintf("Pubcoin=%s\n", pubCoin.ToString());
                    // CHECKING PROCESS
                    for(const CZerocoinEntry &pubCoinItem: listPubCoin) {
                        if(nDepth < 1 && pubCoin == pubCoinItem.value){
                            walletdb.EraseZerocoinEntry(pubCoinItem);
                            continue;
                        }
                    }

                }
            }
        }
    }
    UniValue results(UniValue::VARR);
    results.push_back("Sucessfully erased unconfirmed zerocoins");

    return results;
}


UniValue listallserials(const JSONRPCRequest& request)
{

    if (request.fHelp || request.params.size() > 1)
        throw runtime_error(
                "listallserials height(default=current_height)\n"
                        "\Lists all zerocoin serials spent from height\n");

    UniValue results(UniValue::VARR);
    if(request.params.size() > 0){
        CBlockIndex *temp = chainActive[request.params[0].get_int()];
        for(auto it = temp->spentSerials.begin(); it != temp->spentSerials.end(); it++){
            results.push_back(it->ToString());
        }
        return results;
    }
    CZerocoinState *zcState = CZerocoinState::GetZerocoinState();

    for(auto it = 53000; it <= chainActive.Tip()->nHeight; it++){
        CBlockIndex *temp = chainActive[it];
        for(auto it = temp->spentSerials.begin(); it != temp->spentSerials.end(); it++){
            results.push_back(it->ToString());
        }
    }
    /*
    for(auto it = zcState->usedCoinSerials.begin(); it != zcState->usedCoinSerials.end(); it++) {
        results.push_back(it->ToString());
    }
    */
    return results;
}

UniValue eraseusedzerocoindata(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() > 1)
        throw runtime_error(
                "eraseunusedzerocoindata\n"
                        "\Erase zerocoin metadata from spent zerocoins\n");

    CWallet *pwalletMain = GetWalletForJSONRPCRequest(request);
    int i = 0;

    {
        LOCK(pwalletMain->cs_wallet);
        list <CZerocoinEntry> listPubCoin = list<CZerocoinEntry>();
        CWalletDB walletdb(pwalletMain->GetDBHandle());
        walletdb.ListPubCoin(listPubCoin);
        for(const CZerocoinEntry &pubCoinItem: listPubCoin) {
            if(pubCoinItem.IsUsed == true){
                walletdb.EraseZerocoinEntry(pubCoinItem);
                i++;
            }
        }
    }

    UniValue results(UniValue::VARR);
    results.push_back("Sucessfully removed " + std::to_string(i) +  " used zerocoin objects from wallet.dat");
    return results;
}

UniValue encryptallzerocoins(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() > 0)
        throw runtime_error(
                "encryptallzerocoins\n"
                        "\Encrypt all zerocoin data\n");

    CWallet *pwalletMain = GetWalletForJSONRPCRequest(request);
    int i = 0;
    {
        if(pwalletMain->IsLocked()){
            throw JSONRPCError(RPC_WALLET_UNLOCK_NEEDED, "Error: Please enter the wallet passphrase with walletpassphrase first.");
        }
        LOCK(pwalletMain->cs_wallet);
        list <CZerocoinEntry> listPubCoin = list<CZerocoinEntry>();
        CWalletDB walletdb(pwalletMain->GetDBHandle());
        walletdb.ListPubCoin(listPubCoin);
        for(const CZerocoinEntry &pubCoinItem: listPubCoin) {

            CZerocoinEntry encryptedZerocoin = pubCoinItem;
            //Zerocoin object is already encrypted
            if(pubCoinItem.ecdsaSecretKey.size() > 32)
                continue;
            //walletdb.EraseZerocoinEntry(pubCoinItem);

            pwalletMain->EncryptPrivateZerocoinData(encryptedZerocoin);
            walletdb.WriteZerocoinEntry(encryptedZerocoin);
            i++;
        }
    }

    UniValue results(UniValue::VARR);
    results.push_back("Encrypted " + std::to_string(i) + " zerocoins");
    return results;
}

UniValue decryptallzerocoins(const JSONRPCRequest& request)
{

    if (request.fHelp || request.params.size() > 0)
        throw runtime_error(
                "decryptallzerocoins\n"
                        "\Decrypt all encrypted zerocoin data\n");

    CWallet *pwalletMain = GetWalletForJSONRPCRequest(request);
    int i = 0;
    {
        if(pwalletMain->IsLocked()){
            throw JSONRPCError(RPC_WALLET_UNLOCK_NEEDED, "Error: Please enter the wallet passphrase with walletpassphrase first.");
        }
        LOCK(pwalletMain->cs_wallet);
        list <CZerocoinEntry> listPubCoin = list<CZerocoinEntry>();
        CWalletDB walletdb(pwalletMain->GetDBHandle());
        walletdb.ListPubCoin(listPubCoin);
        for(const CZerocoinEntry &pubCoinItem: listPubCoin) {

            CZerocoinEntry decryptedZerocoin = pubCoinItem;
            //Zerocoin object is not encrypted
            if(pubCoinItem.ecdsaSecretKey.size() <= 32)
                continue;

            pwalletMain->DecryptPrivateZerocoinData(decryptedZerocoin);
            walletdb.WriteZerocoinEntry(decryptedZerocoin);
            i++;
        }
    }

    UniValue results(UniValue::VARR);
    results.push_back("Decrypted " + std::to_string(i) + " zerocoins");
    return results;
}

UniValue getstakingaverage(const JSONRPCRequest& request)
{

    if (request.fHelp || request.params.size() > 0)
        throw runtime_error(
                "getstakingaverage\n"
                        "\Get the average stake amount in the last 500 block sample.\n");

    UniValue entry(UniValue::VOBJ);
    if(IsInitialBlockDownload())
        return "Wait until node is fully synced.";

    int sample = 500;
    vector<int64_t> stakeVector;
    stakeVector.clear();
    if(chainActive.Tip()->nHeight < sample)
        sample = chainActive.Tip()->nHeight;
    int startHeight = chainActive.Tip()->nHeight - sample;
    for(auto it = startHeight; it < chainActive.Tip()->nHeight; it++){
        CBlock block;
        CBlockIndex *pindex = chainActive[it];
        // check level 0: read from disk
        if (ReadBlockFromDisk(block, pindex, Params().GetConsensus())){
            stakeVector.push_back(block.vtx[0]->vout[0].nValue);
        }
    }

    int64_t averageStake = 0;
    for(int i = 0; i < stakeVector.size(); i++)
        averageStake += stakeVector[i];

    entry.push_back(Pair("average_stake_amount", (averageStake/stakeVector.size())/COIN));

    return entry;
}

#include <ghostnode/ghostnodeman.h>
UniValue ghostfeepayouttotal(const JSONRPCRequest& request)
{

    if (request.fHelp || request.params.size() > 0)
        throw runtime_error(
                "ghostfeepayouttotal\n"
                        "\Get the ghostfee payout total in the upcoming cycle.\n");

    UniValue entry(UniValue::VOBJ);
    if(IsInitialBlockDownload())
        return "Wait until node is fully synced.";

    CAmount returnFee = 0;
    CAmount totalGhosted = 0;
    vector<CAmount> mintVector;
    mintVector.clear();

    int totalCount = (chainActive.Height() + 1) % Params().GetConsensus().nGhostFeeDistributionCycle;
    //Subtract 1 from sample since we check current block fees
    mintVector.clear();

    //Assume chainactive+1 is current block check height
    int startHeight = chainActive.Height() + 1 - totalCount;
    //Grab fee from other blocks
    for(auto it = startHeight; it < chainActive.Height() + 1; it++){
        CBlock block;
        CBlockIndex *pindex = chainActive[it];
        // Now get fees from past 719 blocks
        if (ReadBlockFromDisk(block, pindex, Params().GetConsensus())){
            for(auto ctx: block.vtx){
                //Found ghost fee transaction
                bool isSpend = ctx->IsZerocoinSpend() || ctx->IsSigmaSpend();
                bool isMint = ctx->IsZerocoinMint() || ctx->IsSigmaMint();

                if(!isSpend && isMint){
                    for(auto mintTx: ctx->vout){
                        if(mintTx.scriptPubKey.IsZerocoinMint() || mintTx.scriptPubKey.IsSigmaMint())
                            mintVector.push_back(mintTx.nValue);
                    }
                }
                if(isSpend && isMint){
                    CAmount inVal = 0;
                    CAmount outVal = 0;
                    for(int k = 0; k < ctx->vout.size(); k++){
                        if(!ctx->vout[k].scriptPubKey.IsSigmaMint())
                            continue;
                        outVal += ctx->vout[k].nValue;
                    }
                    // add input denoms
                    for(int k = 0; k < ctx->vin.size(); k++){
                        std::pair<std::unique_ptr<sigma::CoinSpend>, uint32_t> newSpend;
                        newSpend = ParseSigmaSpend(ctx->vin[k]);
                        inVal += newSpend.first->getIntDenomination();
                    }
                    CAmount neededForFee = (inVal - outVal)/0.0025;
                    mintVector.push_back(neededForFee);
                }
            }
        }
        else
            return "ReadBlockFromDisk failed!";
    }

    for(auto f: mintVector)
        totalGhosted += f;
    //Calculate total fees for the 720 block cycle
    returnFee = totalGhosted * 0.0025;

    vector<CGhostnode> ghostnodeVector = mnodeman.GetFullGhostnodeVector();

    int totalActiveNodes = 0;
    int64_t ensureNodeActiveBefore = chainActive[startHeight]->GetBlockTime();

    for(auto node: ghostnodeVector){

        if(node.IsEnabled() && (node.sigTime <= ensureNodeActiveBefore))
            totalActiveNodes++;
    }


    entry.push_back(Pair("ghost_fee_payout", ValueFromAmount(returnFee)));
    entry.push_back(Pair("total_active_nodes", (totalActiveNodes)));

    return entry;
}

UniValue ghostprivacysets(const JSONRPCRequest& request)
{

    if (request.fHelp || request.params.size() > 0)
        throw runtime_error(
                "ghostprivacysets\n"
                        "\Get the total ghosted denomination amounts in the network.\n");

    if(IsInitialBlockDownload())
        return "Wait until node is fully synced.";

    UniValue entry(UniValue::VOBJ);

    int mintVector[8] = {0,0,0,0,0,0,0,0};

    //Ghostprotocol active since 53k
    int startHeight = 53000;
    //Grab fee from other blocks
    for(auto it = startHeight; it < chainActive.Height() + 1; it++){
        CBlock block;
        CBlockIndex *pindex = chainActive[it];
        // Now get fees from past 719 blocks
        if (ReadBlockFromDisk(block, pindex, Params().GetConsensus())){
            for(auto ctx: block.vtx){
                //Found ghost fee transaction
                if(ctx->IsZerocoinMint()){
                    for(auto mintTx: ctx->vout){
                        if(mintTx.scriptPubKey.IsZerocoinMint()){
                            if(mintTx.nValue == 1 * COIN)
                                mintVector[0]++;
                            else if(mintTx.nValue == 5 * COIN)
                                mintVector[1]++;
                            else if(mintTx.nValue == 10 * COIN)
                                mintVector[2]++;
                            else if(mintTx.nValue == 50 * COIN)
                                mintVector[3]++;
                            else if(mintTx.nValue == 100 * COIN)
                                mintVector[4]++;
                            else if(mintTx.nValue == 500 * COIN)
                                mintVector[5]++;
                            else if(mintTx.nValue == 1000 * COIN)
                                mintVector[6]++;
                            else if(mintTx.nValue == 5000 * COIN)
                                mintVector[7]++;
                        }
                    }
                }

                //Found ghost fee transaction
                if(ctx->IsZerocoinSpend()){
                    for(auto mintTx: ctx->vout){
                        if(mintTx.nValue == 1 * COIN)
                            mintVector[0]--;
                        else if(mintTx.nValue == 5 * COIN)
                            mintVector[1]--;
                        else if(mintTx.nValue == 10 * COIN)
                            mintVector[2]--;
                        else if(mintTx.nValue == 50 * COIN)
                            mintVector[3]--;
                        else if(mintTx.nValue == 100 * COIN)
                            mintVector[4]--;
                        else if(mintTx.nValue == 500 * COIN)
                            mintVector[5]--;
                        else if(mintTx.nValue == 1000 * COIN)
                            mintVector[6]--;
                        else if(mintTx.nValue == 5000 * COIN)
                            mintVector[7]--;
                    }
                }
            }
        }
        else
            return "ReadBlockFromDisk failed!";
    }

    CAmount total = (mintVector[0] * 1) + (mintVector[1] * 5) + (mintVector[2] * 10) +
            (mintVector[3] * 50) + (mintVector[4] * 100) + (mintVector[5] * 500) +
            (mintVector[6] * 1000) + (mintVector[7] * 5000);

    entry.push_back(Pair("1", (mintVector[0])));
    entry.push_back(Pair("5", (mintVector[1])));
    entry.push_back(Pair("10", (mintVector[2])));
    entry.push_back(Pair("50", (mintVector[3])));
    entry.push_back(Pair("100", (mintVector[4])));
    entry.push_back(Pair("500", (mintVector[5])));
    entry.push_back(Pair("1000", (mintVector[6])));
    entry.push_back(Pair("5000", (mintVector[7])));
    entry.push_back(Pair("total", total));


    return entry;
}


UniValue ghostprivacysetsv2(const JSONRPCRequest& request)
{

    if (request.fHelp || request.params.size() > 0)
        throw runtime_error(
                "ghostprivacysetsv2\n"
                        "\Get the total ghosted denomination amounts in the network.\n");

    if(IsInitialBlockDownload())
        return "Wait until node is fully synced.";

    UniValue entry(UniValue::VOBJ);

    int mintVector[6] = {0,0,0,0,0,0};

    //Ghostprotocol active since 53k
    int startHeight = Params().GetConsensus().nSigmaStartBlock;
    //Grab fee from other blocks
    for(auto it = startHeight; it < chainActive.Height() + 1; it++){
        CBlock block;
        CBlockIndex *pindex = chainActive[it];
        if (ReadBlockFromDisk(block, pindex, Params().GetConsensus())){
            for(auto ctx: block.vtx){
                //Found ghost fee transaction
                if(ctx->IsSigmaMint()){
                    for(auto mintTx: ctx->vout){
                        if(mintTx.scriptPubKey.IsSigmaMint()){
                            if(mintTx.nValue == 10 * CENT)
                                mintVector[0]++;
                            else if(mintTx.nValue == 1 * COIN)
                                mintVector[1]++;
                            else if(mintTx.nValue == 10 * COIN)
                                mintVector[2]++;
                            else if(mintTx.nValue == 100 * COIN)
                                mintVector[3]++;
                            else if(mintTx.nValue == 1000 * COIN)
                                mintVector[4]++;
                            else if(mintTx.nValue == 10000 * COIN)
                                mintVector[5]++;
                        }
                    }
                }

                //Found ghost fee transaction
                if(ctx->IsSigmaSpend()){
                    for(auto mintTx: ctx->vout){
                        if(mintTx.nValue == 10 * CENT)
                            mintVector[0]--;
                        else if(mintTx.nValue == 1 * COIN)
                            mintVector[1]--;
                        else if(mintTx.nValue == 10 * COIN)
                            mintVector[2]--;
                        else if(mintTx.nValue == 100 * COIN)
                            mintVector[3]--;
                        else if(mintTx.nValue == 1000 * COIN)
                            mintVector[4]--;
                        else if(mintTx.nValue == 10000 * COIN)
                            mintVector[5]--;
                    }
                }
            }
        }
        else
            return "ReadBlockFromDisk failed!";
    }

    CAmount total = ((double)mintVector[0] * 0.1) + (mintVector[1]) + (mintVector[2] * 10) +
            (mintVector[3] * 100) + (mintVector[4] * 1000) + (mintVector[5] * 10000);

    entry.push_back(Pair("0.1", (mintVector[0])));
    entry.push_back(Pair("1", (mintVector[1])));
    entry.push_back(Pair("10", (mintVector[2])));
    entry.push_back(Pair("100", (mintVector[3])));
    entry.push_back(Pair("1000", (mintVector[4])));
    entry.push_back(Pair("10000", (mintVector[5])));
    entry.push_back(Pair("total", total));

    return entry;
}

UniValue mintghostdata(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() > 1)
        throw runtime_error("mintghostdata <amount>(1,5,10,50,100,500,1000,5000)\n" );

    UniValue entry(UniValue::VOBJ);
    int64_t nAmount = 0;
    libzerocoin::CoinDenomination denomination;
    // Amount
    if (request.params[0].get_real() == 1.0) {
        denomination = libzerocoin::ZQ_ONE;
        nAmount = AmountFromValue(request.params[0]);
    } else if (request.params[0].get_real() == 5.0) {
        denomination = libzerocoin::ZQ_FIVE;
        nAmount = AmountFromValue(request.params[0]);
    } else if (request.params[0].get_real() == 10.0) {
        denomination = libzerocoin::ZQ_TEN;
        nAmount = AmountFromValue(request.params[0]);
    } else if (request.params[0].get_real() == 50.0) {
        denomination = libzerocoin::ZQ_FIFTY;
        nAmount = AmountFromValue(request.params[0]);
    } else if (request.params[0].get_real() == 100.0) {
        denomination = libzerocoin::ZQ_ONE_HUNDRED;
        nAmount = AmountFromValue(request.params[0]);
    } else if (request.params[0].get_real() == 500.0) {
        denomination = libzerocoin::ZQ_FIVE_HUNDRED;
        nAmount = AmountFromValue(request.params[0]);
    } else if (request.params[0].get_real() == 1000.0) {
        denomination = libzerocoin::ZQ_ONE_THOUSAND;
        nAmount = AmountFromValue(request.params[0]);
    } else if (request.params[0].get_real() == 5000.0) {
        denomination = libzerocoin::ZQ_FIVE_THOUSAND;
        nAmount = AmountFromValue(request.params[0]);
    } else {
        throw runtime_error("mintghostdata <amount>(1,5,10,50,100,500,1000,5000)\n");
    }

    // Set up the Zerocoin Params object
    libzerocoin::Params *zcParams = ZCParams;

    int mintVersion = 1;

    // The following constructor does all the work of minting a brand
    // new zerocoin. It stores all the private values inside the
    // PrivateCoin object. This includes the coin secrets, which must be
    // stored in a secure location (wallet) at the client.
    libzerocoin::PrivateCoin newCoin(zcParams, denomination, mintVersion);

    // Get a copy of the 'public' portion of the coin. You should
    // embed this into a Zerocoin 'MINT' transaction along with a series
    // of currency inputs totaling the assigned value of one zerocoin.
    libzerocoin::PublicCoin pubCoin = newCoin.getPublicCoin();

    // Validate
    if (pubCoin.validate()) {

        UniValue pub_data(UniValue::VOBJ);
        UniValue priv_data(UniValue::VOBJ);
        pub_data.push_back(Pair("size", (uint64_t)pubCoin.getValue().getvch().size()));
        pub_data.push_back(Pair("pubcoin", (pubCoin.getValue().GetHex())));
        pub_data.push_back(Pair("amount", (denomination)));


        const unsigned char *ecdsaSecretKey = newCoin.getEcdsaSeckey();
        std::vector<unsigned char> seckey = std::vector<unsigned char>(ecdsaSecretKey, ecdsaSecretKey+32);
        priv_data.push_back(Pair("seckey", (CBigNum(seckey).GetHex())));
        priv_data.push_back(Pair("randomness", (newCoin.getRandomness().GetHex())));
        priv_data.push_back(Pair("serial", (newCoin.getSerialNumber().GetHex())));

        entry.push_back(Pair("pub_data", (pub_data)));
        entry.push_back(Pair("priv_data", (priv_data)));

    } else {
        return "pubCoin.validate() failed\n";
    }

    return entry;
}

UniValue spendghostdata(const JSONRPCRequest& request) {

    CWallet * const pwalletMain = GetWalletForJSONRPCRequest(request);

    if (request.fHelp || request.params.size() != 6)
        throw runtime_error(
                "spendghostdata <amount>(1,5,10,50,100,500,1000,5000), <seckey>, <randomness>, <serial>, <pubValue>, <spendtoaddress> \n");


    int64_t nAmount = 0;
    libzerocoin::CoinDenomination denomination;
    // Amount
    if (request.params[0].get_real() == 1.0) {
        denomination = libzerocoin::ZQ_ONE;
        nAmount = AmountFromValue(request.params[0]);
    } else if (request.params[0].get_real() == 5.0) {
        denomination = libzerocoin::ZQ_FIVE;
        nAmount = AmountFromValue(request.params[0]);
    } else if (request.params[0].get_real() == 10.0) {
        denomination = libzerocoin::ZQ_TEN;
        nAmount = AmountFromValue(request.params[0]);
    } else if (request.params[0].get_real() == 50.0) {
        denomination = libzerocoin::ZQ_FIFTY;
        nAmount = AmountFromValue(request.params[0]);
    } else if (request.params[0].get_real() == 100.0) {
        denomination = libzerocoin::ZQ_ONE_HUNDRED;
        nAmount = AmountFromValue(request.params[0]);
    } else if (request.params[0].get_real() == 500.0) {
        denomination = libzerocoin::ZQ_FIVE_HUNDRED;
        nAmount = AmountFromValue(request.params[0]);
    } else if (request.params[0].get_real() == 1000.0) {
        denomination = libzerocoin::ZQ_ONE_THOUSAND;
        nAmount = AmountFromValue(request.params[0]);
    } else if (request.params[0].get_real() == 5000.0) {
        denomination = libzerocoin::ZQ_FIVE_THOUSAND;
        nAmount = AmountFromValue(request.params[0]);
    } else {
        throw runtime_error("spendghostdata <amount>(1,5,10,50,100,500,1000,5000), <seckey>, <randomness>, <serial>, <pubValue>, <spendtoaddress>\n");
    }

    CBitcoinAddress address;

    CBigNum seckey = CBigNum(request.params[1].get_str().c_str());
    CBigNum randomness = CBigNum(request.params[2].get_str().c_str());
    CBigNum serial = CBigNum(request.params[3].get_str().c_str());
    CBigNum pubValue = CBigNum(request.params[4].get_str().c_str());

    // Address
    address = CBitcoinAddress(request.params[5].get_str());

    if (!address.IsValid())
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "rpcwallet spendghostdata(): Invalid spendtoaddress address");

    /*
    UniValue pub_data(UniValue::VOBJ);
    pub_data.push_back(Pair("amount", nAmount));
    pub_data.push_back(Pair("address", address.ToString()));

    pub_data.push_back(Pair("seckey", seckey.GetHex()));
    pub_data.push_back(Pair("randomness", randomness.GetHex()));
    pub_data.push_back(Pair("serial", serial.GetHex()));

    return pub_data;
    */

    std::string strError = "";
    pwalletMain->SpendGhostData(denomination, address, seckey, randomness, serial, pubValue, strError);

    return strError;
}

UniValue getzerocoinacc(const JSONRPCRequest& request)
{
    if (request.fHelp)
        throw runtime_error(
                "getzerocoinacc \n");

    UniValue entry(UniValue::VOBJ);

    CZerocoinState *zerocoinState = CZerocoinState::GetZerocoinState();

    std::vector<CBigNum> accValues;
    accValues.clear();
    std::vector<uint256> accBlockHashes;
    accBlockHashes.clear();
    zerocoinState->GetWitnessForAllSpends(accValues, accBlockHashes);

    entry.push_back(Pair("1", (accValues[0].GetHex())));
    entry.push_back(Pair("5", (accValues[1].GetHex())));
    entry.push_back(Pair("10", (accValues[2].GetHex())));
    entry.push_back(Pair("50", (accValues[3].GetHex())));
    entry.push_back(Pair("100", (accValues[4].GetHex())));
    entry.push_back(Pair("500", (accValues[5].GetHex())));
    entry.push_back(Pair("1000", (accValues[6].GetHex())));
    entry.push_back(Pair("5000", (accValues[7].GetHex())));

    //return "null";
    return entry;
}

#include <netmessagemaker.h>
UniValue getdatazerocoinacc(const JSONRPCRequest& request)
{
    if (request.fHelp)
        throw runtime_error(
                "getzerocoinacc \n");

    UniValue entry(UniValue::VOBJ);


    if (g_connman) {
        // hash is not used
        // to send (2*n): pubcoin height & denomination
        // receive (3*n): witness & accval & accval blockhash
        std::vector<CInv> vGetData;
        CInv invHeight(MSG_ZEROCOIN_ACC, uint256S("0x100"));
        CInv invDenom(MSG_ZEROCOIN_ACC, uint256S("0xa"));
        vGetData.push_back(invHeight);
        vGetData.push_back(invDenom);
        g_connman->ForEachNode([&](CNode* pnode)
        {
            const CNetMsgMaker msgMaker(pnode->GetSendVersion());
            g_connman->PushMessage(pnode, msgMaker.Make(NetMsgType::GETDATA, vGetData));
        });
        LogPrintf("Relaying get ZCACC to peers \n");
    }

    return "null";
}

#include "governance/networking-governance.h"

UniValue getoffchainproposals(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() > 0)
        throw runtime_error("getoffchainproposals \n");

    g_governance.SendRequests(RequestTypes::GET_PROPOSALS);

    while(!g_governance.isReady()){}

    // store vote only on successfull request
    if(!g_governance.statusOK){
        return "error, cannot get proposal list";
    }

    UniValue end(UniValue::VOBJ);
    for(int i = 0; i < g_governance.proposals.size(); i++){
        end.pushKV("Proposal " + std::to_string(i), g_governance.proposals[i].toJSONString());
    }


    return end;
}

/* Example post:
 * {"voteid":"test123","address":"GNqLhRRh3eVBBDrWsGJNgX8nWQt1zciJPu",
 * "signature":"IE+oq0+L4+JAcOMztJLebEp7xR8hs/v2rXCW/slNcYQsH0ElchOI1xnPIANNWAg01KmqbAcfUjjHPotjyjuSkXU=",
 * "ballot":0}
 */
UniValue postoffchainproposals(const JSONRPCRequest& request)
{
    CWallet *pwallet = GetWalletForJSONRPCRequest(request);

    if (request.fHelp || request.params.size() != 2)
        throw runtime_error("getoffchainproposals vote_id decision(0/1)\n"
                            + HelpRequiringPassphrase(pwallet) + "\n"
                            "\nArguments:\n"
                            "1. \"vote_id\"         (string, required) The vote ID of the proposal this wallet is voting for \"\".\n"
                            "2. \"decision\"        (string, required) The decision of this wallet's vote. Binary value, 0 = against, 1 = in favor.\n");

    LOCK2(cs_main, pwallet->cs_wallet);

    EnsureWalletIsUnlocked(pwallet);

    CWalletDB walletdb(pwallet->GetDBHandle());

    std::string postMessage;
    std::string vote_id = request.params[0].get_str();
    std::string decision = request.params[1].get_str();


    std::list<CGovernanceEntry> govEntries;
    walletdb.ListGovernanceEntries(govEntries);

    for(auto entry: govEntries){
        // make sure we are not voting for a proposal we have voted for already
        if(vote_id == entry.voteID){
            throw JSONRPCError(RPC_TYPE_ERROR, "You have already voted for this proposal!\nYour vote weight: " + std::to_string(entry.voteWeight));
        }
    }


    // timeframe we should check for transactions
    // anything not within the limit is assumed to be a weight of 0
    // use 46 days (30 days prior for eligibility + 15 days for voting + 1 cushion)
    const int64_t vote_timeframe = 46 * 60 * 60 * 24;
    const std::time_t current_time = std::time(0);

    std::vector<CScript> votingAddresses;
    votingAddresses.clear();
    // Cycle through all transactions and log all addresses
    for (auto& mapping: pwallet->mapWallet){
        CWalletTx wtx = mapping.second;

        if(!wtx.IsCoinStake() || wtx.GetTxTime() < (current_time - vote_timeframe))
            continue;


        // check for multiple outputs
        for(auto& vout: wtx.tx->vout){

            if (!::IsMine(*pwallet, vout.scriptPubKey)) continue;

            // skip p2sh, only bech32/legacy allowed
            if(vout.scriptPubKey.IsPayToScriptHashAny())
                continue;

            if (std::find(votingAddresses.begin(), votingAddresses.end(), vout.scriptPubKey) != votingAddresses.end())
                continue;

            // store unique values
            votingAddresses.push_back(vout.scriptPubKey);
        }

    }

    postMessage = "[";

    int id = 0;

    for(auto &addrScript: votingAddresses){

        CTxDestination dest;
        ExtractDestination(addrScript, dest);

        std::string strAddress = EncodeDestination(dest);
        std::string strMessage = vote_id + "_" + decision;

        if (!IsValidDestination(dest)) {
            throw JSONRPCError(RPC_TYPE_ERROR, "Invalid address");
        }

        const CKeyID keyID = GetKeyForDestination(*pwallet, dest);
        if (keyID.IsNull()) {
            throw JSONRPCError(RPC_TYPE_ERROR, "Address does not refer to key");
        }

        CKey key;
        if (!pwallet->GetKey(keyID, key)) {
            throw JSONRPCError(RPC_WALLET_ERROR, "Private key not available");
        }

        CHashWriter ss(SER_GETHASH, 0);
        ss << strMessageMagic;
        ss << strMessage;

        std::vector<unsigned char> vchSig;
        if (!key.SignCompact(ss.GetHash(), vchSig))
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Sign failed");

        if(id != 0)
            postMessage += ",";

        postMessage += "{"
                       "\"voteid\":\"" + vote_id +
                        "\",\"address\":\"" + strAddress +
                        "\",\"signature\":\"" + EncodeBase64(vchSig.data(), vchSig.size()) +
                        "\",\"ballot\":\"" + decision + "\"}";

        id++;

    }


    postMessage += "]";

    g_governance.SendRequests(RequestTypes::CAST_VOTE, postMessage);

    while(!g_governance.isReady()){}

    // store vote only on successfull request
    if(!g_governance.statusOK){
        return "error, vote not casted";
    }

    UniValue end(UniValue::VOBJ);

    CAmount voteWeight = 0;
    for(int i = 0; i < g_governance.votes.size(); i++){
        if(g_governance.votes[i].vote_id != vote_id)
            continue;

        end.pushKV("Vote " + std::to_string(i), g_governance.votes[i].toJSONString());

        voteWeight += std::stoi(g_governance.votes[i].weight);
    }

    if(voteWeight != 0){
        // place vote into wallet db for future reference
        CGovernanceEntry govVote;
        govVote.voteID = vote_id;
        govVote.voteWeight = voteWeight;
        walletdb.WriteGovernanceEntry(govVote);
    }

    return end;
}

UniValue getvoteweight(const JSONRPCRequest& request)
{
    CWallet *pwallet = GetWalletForJSONRPCRequest(request);

    if (request.fHelp || request.params.size() != 2)
        throw runtime_error("getvoteweight start_time end_time\n"
                            + HelpRequiringPassphrase(pwallet) + "\n"
                            "\nArguments:\n"
                            "1. \"start_time\"         (int, required) The starting time (unix) for the weight calculation.\n"
                            "2. \"end_time\"        (int, required) The ending time (unix) for the weight calculation.\n");

    LOCK2(cs_main, pwallet->cs_wallet);

    UniValue end(UniValue::VOBJ);

    int64_t start_time = request.params[0].get_int64();
    int64_t end_time = request.params[1].get_int64();

    // Cycle through all transactions and log all addresses
    std::vector<CScript> votingAddresses;
    votingAddresses.clear();

    CAmount nVoteWeight = 0;
    for (auto& mapping: pwallet->mapWallet){
        CWalletTx wtx = mapping.second;

        if(!(wtx.IsCoinStake() && wtx.IsInMainChain()
             && wtx.GetTxTime() >= start_time && wtx.GetTxTime() <= end_time))
            continue;

        // check for multiple outputs
        for(auto& vout: wtx.tx->vout){

            if (!::IsMine(*pwallet, vout.scriptPubKey)) continue;

            // skip p2sh, only bech32/legacy allowed
            if(vout.scriptPubKey.IsPayToScriptHashAny())
                continue;

            // check roughly how much staking rewards have been earned
            // verification requres 'getaddressvoteweight'
            if(vout.scriptPubKey.IsPayToWitnessKeyHash_CS() || vout.scriptPubKey.IsPayToWitnessKeyHash()){
                int height = wtx.fHeightCached ? wtx.nCachedHeight : chainActive.Height();
                CBlockIndex *pindexPrev = chainActive[height];
                nVoteWeight += Params().GetProofOfStakeReward(pindexPrev, 0);
            }
            else
                nVoteWeight += vout.nValue;
        }
    }

    end.pushKV("vote_weight", ((double)nVoteWeight/COIN));
    return end;
}

UniValue erasegoventries(const JSONRPCRequest& request)
{
    CWallet *pwallet = GetWalletForJSONRPCRequest(request);

    if (request.fHelp || request.params.size() > 0)
        throw runtime_error("eraseallgoventires \n"
                            "Erase all wallet database voting entries for the current local wallet. \n"
                            + HelpRequiringPassphrase(pwallet));

    LOCK2(cs_main, pwallet->cs_wallet);

    EnsureWalletIsUnlocked(pwallet);

    CWalletDB walletdb(pwallet->GetDBHandle());

    std::list<CGovernanceEntry> govEntries;
    walletdb.ListGovernanceEntries(govEntries);
    int i = 0;
    for(auto &entry: govEntries){
        // make sure we are not voting for a proposal we have voted for already
        if(!walletdb.EraseGovernanceEntry(entry)){
            throw JSONRPCError(RPC_TYPE_ERROR, "WalletDB::EraseGovernanceEntry failed!");
        }
        i++;
    }

    UniValue end(UniValue::VOBJ);
    end.pushKV("entries_erased", i);
    return end;
}

#include <zerocoin/sigma.h>

UniValue getpubcoinpackv2(const JSONRPCRequest& request) {

    if (request.fHelp || request.params.size() > 1)
        throw runtime_error(
                "getpubcoinpackv2 amount(default=10)\n"
                "\nResults a Commitment Key Pack\n");


    CWallet * const pwalletMain = GetWalletForJSONRPCRequest(request);

    EnsureWalletIsUnlocked(pwalletMain);

    UniValue results(UniValue::VARR);


    int keyAmount = 10;
    if(request.params.size() > 0)
        keyAmount = request.params[0].get_int();

    sigma::Params *sParam = SParams;
    // get latest unused mints
    vector<sigma::PrivateCoin> privCoins;
    int i = pwalletMain->GetGhostWallet()->GetCount();
    int original = keyAmount + i;
    for(i = i; i < original; i++){
        // Regenerate the mint
        CSigmaMint dMint;
        sigma::PrivateCoin coin(sParam, sigma::CoinDenomination::SIGMA_0_1, sigma::SIGMA_VERSION_2);
        pwalletMain->GetGhostWallet()->GenerateHDMint(sigma::CoinDenomination::SIGMA_0_1, coin, dMint);
        if(!coin.getPublicCoin().validate())
            continue;

        //write mint to DB, will get scanned if ckp pay is made
        CWalletDB(pwalletMain->GetDBHandle()).WriteSigmaMint(dMint);
        privCoins.push_back(coin);
        pwalletMain->GetGhostWallet()->UpdateCountLocal();
    }
    //reset count
    pwalletMain->GetGhostWallet()->SetCount(original - keyAmount);

    std::vector< std::vector <unsigned char>> keyList = std::vector< std::vector <unsigned char>>();
    keyList.clear();
    for(const sigma::PrivateCoin &pCoin: privCoins) {
        std::vector<unsigned char> commitmentKey = pCoin.getPublicCoin().getValue().getvch();
        keyList.push_back(commitmentKey);
    }

    CommitmentKeyPack pubCoinPack(keyList);

    results.push_back(pubCoinPack.GetPubCoinPackDataBase58());

    return results;
}

UniValue ghostamountv2(const JSONRPCRequest& request)
{

    CWallet *pwalletMain = GetWalletForJSONRPCRequest(request);

    if (request.fHelp || request.params.size() > 2)
        throw runtime_error("ghostamountv2 <amount>(whole numbers only) <commitment_key_pack>(optional)\n" + HelpRequiringPassphrase(pwalletMain));


    if (!IsSigmaAllowed()) {
        throw JSONRPCError(RPC_WALLET_ERROR, "Sigma is not activated yet");
    }


    CAmount nAmount = AmountFromValue(request.params[0]);
    LogPrintf("RPCWallet::ghostamountv2(): denomination = %s, nAmount = %d \n", request.params[0].getValStr(), nAmount);

    std::vector<sigma::PrivateCoin> privCoins;
    privCoins.clear();
    std::string strError;
    if(!pwalletMain->CreateSigmaMints(nAmount, privCoins, strError))
        throw JSONRPCError(RPC_WALLET_ERROR, strError);

    std::vector<CScript> keypack;
    keypack.clear();
    if(!request.params[1].isNull()){
        std::string k = request.params[1].get_str();
        CommitmentKeyPack keys(k);
        if(!keys.IsValidPack())
            throw JSONRPCError(RPC_WALLET_ERROR, "invalid commitment key pack");
        keypack = keys.GetPubCoinPackScript();
        if(privCoins.size() > keypack.size())
            throw JSONRPCError(RPC_WALLET_ERROR, "pubcoin pack too small, need at least: " +
                               std::to_string(privCoins.size()) + ", have only: " + std::to_string(keypack.size()));
    }

    vector<CSigmaMint> vDMints;
    auto vecSend = pwalletMain->CreateSigmaMintRecipients(privCoins, vDMints, keypack);

    CWalletTx wtx;
    strError = pwalletMain->MintAndStoreSigma(vecSend, privCoins, vDMints, wtx, false);

    if (strError != "")
        throw JSONRPCError(RPC_WALLET_ERROR, strError);

    return wtx.GetHash().GetHex();
}

UniValue unghostamountv2(const JSONRPCRequest& request)
{
    CWallet *pwalletMain = GetWalletForJSONRPCRequest(request);

    if (request.fHelp || request.params.size() == 0 || request.params.size() > 2)
        throw runtime_error("unghostamountv2 <amount>(whole numbers only) <addresstosend>(either address or commitment key pack)\n" + HelpRequiringPassphrase(pwalletMain));

    if (!IsSigmaAllowed()) {
        throw JSONRPCError(RPC_WALLET_ERROR, "Sigma is not activated yet");
    }

    std::string nAmount = request.params[0].get_str();

    CTxDestination dest;
    std::string toKey = "";
    std::vector <CScript> keyList;
    keyList.clear();
    if (request.params.size() > 1){
        // Address
        toKey = request.params[1].get_str();
        CommitmentKeyPack keypack(toKey);
        dest = DecodeDestination(toKey);
        if(keypack.IsValidPack()){
            keyList = keypack.GetPubCoinPackScript();
            toKey = "";
        }
        else if(!IsValidDestination(dest))
            throw JSONRPCError(RPC_WALLET_ERROR, "invalid key");
    }

    if (pwalletMain->IsLocked())
        throw JSONRPCError(RPC_WALLET_UNLOCK_NEEDED,
                           "Error: Please enter the wallet passphrase with walletpassphrase first.");

    std::string strError = pwalletMain->GhostModeSpendSigma(nAmount, toKey, keyList);


    return strError;
}

UniValue listghostednixv2(const JSONRPCRequest& request) {
    if (request.fHelp || request.params.size() > 1)
        throw runtime_error(
                "listghostednixv2 <all>(false/true)\n"
                        "\nArguments:\n"
                        "1. <all> (boolean, optional) false (default) to return unspent minted sigma coins, true to return every minted sigma coin.\n"
                        "\nResults are an array of Objects, each of which has:\n"
                        "{id, IsUsed, denomination, value, serialNumber, nHeight, randomness}");

    bool fAllStatus = false;
    if (request.params.size() > 0) {
        fAllStatus = request.params[0].get_bool();
    }

    CWallet * const pwalletMain = GetWalletForJSONRPCRequest(request);

    std::list<CMintMeta> mintMetas = pwalletMain->sigmaTracker->GetMints(true);

    UniValue results(UniValue::VARR);

    for(const CMintMeta &mintItem: mintMetas) {
        UniValue entry(UniValue::VOBJ);
        CAmount nVal = 0;
        sigma::DenominationToInteger(mintItem.denom, nVal);
        entry.push_back(Pair("deterministic", mintItem.isDeterministic));
        entry.push_back(Pair("isUsed", mintItem.isUsed));
        entry.push_back(Pair("height", mintItem.nHeight));
        entry.push_back(Pair("denomination", std::to_string(nVal)));
        entry.push_back(Pair("pubcoinValue", mintItem.pubCoinValue.tostring()));
        results.push_back(entry);
    }

    return results;
}

UniValue getsigmaseed(const JSONRPCRequest& request)
{

    CWallet *pwalletMain = GetWalletForJSONRPCRequest(request);

    if(request.fHelp || !request.params.empty())
        throw runtime_error(
            "getsigmaseed\n"
            "\nDump the deterministic sigma seed for all sigma coins\n" +
            HelpRequiringPassphrase(pwalletMain) + "\n"

            "\nResult\n"
            "\"seed\" : s,  (string) The deterministic zPIV seed.\n"

            "\nExamples\n" +
            HelpExampleCli("getsigmaseed", "") + HelpExampleRpc("getsigmaseed", ""));

    EnsureWalletIsUnlocked(pwalletMain);

    CGhostWallet* ghostWallet = pwalletMain->GetGhostWallet();
    uint256 seed = ghostWallet->GetMasterSeed();

    UniValue ret(UniValue::VOBJ);
    ret.push_back(Pair("seed", seed.GetHex()));

    return ret;
}

UniValue setsigmaseed(const JSONRPCRequest& request)
{
    CWallet *pwalletMain = GetWalletForJSONRPCRequest(request);

    if(request.fHelp || request.params.size() != 1)
        throw runtime_error(
            "setsigmaseed \"seed\"\n"
            "\nSet the wallet's deterministic sigma seed to a specific value.\n" +
            HelpRequiringPassphrase(pwalletMain) + "\n"

            "\nArguments:\n"
            "1. \"seed\"        (string, required) The deterministic sigma seed.\n"

            "\nResult\n"
            "\"success\" : b,  (boolean) Whether the seed was successfully set.\n"

            "\nExamples\n" +
            HelpExampleCli("setsigmaseed", "6b54736b13ce6990753b7345a9b41ca2ce5c5847125b49bf3ffa15f47f5001cd") +
            HelpExampleRpc("setsigmaseed", "6b54736b13ce6990753b7345a9b41ca2ce5c5847125b49bf3ffa15f47f5001cd"));

    EnsureWalletIsUnlocked(pwalletMain);

    uint256 seed;
    seed.SetHex(request.params[0].get_str());

    CGhostWallet* ghostWallet = pwalletMain->GetGhostWallet();
    bool fSuccess = ghostWallet->SetMasterSeed(seed, true);
    if (fSuccess)
        ghostWallet->SyncWithChain();

    UniValue ret(UniValue::VOBJ);
    ret.push_back(Pair("success", fSuccess));

    return ret;
}

UniValue listsigmaentries(const JSONRPCRequest& request)
{
    CWallet *pwalletMain = GetWalletForJSONRPCRequest(request);

    if(request.fHelp)
        throw runtime_error(
            "listsigmaentries <true/false>(default = false)\n"
            "\nList sigma entries in wallet.\n" +
            HelpRequiringPassphrase(pwalletMain) + "\n"

            "\nArguments:\n"
            "1. <true/false>   (string, required) Whether to list all entries including spent.\n");

    EnsureWalletIsUnlocked(pwalletMain);

    CWalletDB db(pwalletMain->GetDBHandle());
    std::list<CSigmaMint> listMintsDB = db.ListSigmaMints();

    bool onlyUnspent = true;
    if(request.params.size() > 0)
        onlyUnspent = request.params[1].getBool();

    UniValue final(UniValue::VARR);

    for(auto& mint: listMintsDB){
        UniValue ret(UniValue::VOBJ);
        if(onlyUnspent && mint.IsUsed())
            continue;
        ret.push_back(Pair("isUsed",  mint.IsUsed()));
        ret.push_back(Pair("denom",  mint.GetDenominationValue()));
        ret.push_back(Pair("height",  mint.GetHeight()));
        ret.push_back(Pair("txid",  mint.GetTxHash().GetHex()));
        final.push_back(ret);
    }
    final.pushKV("final_size", std::to_string(listMintsDB.size()));

    return final;
}

extern UniValue abortrescan(const JSONRPCRequest& request); // in rpcdump.cpp
extern UniValue dumpprivkey(const JSONRPCRequest& request); // in rpcdump.cpp
extern UniValue importprivkey(const JSONRPCRequest& request);
extern UniValue importaddress(const JSONRPCRequest& request);
extern UniValue importpubkey(const JSONRPCRequest& request);
extern UniValue dumpwalletprivatekeys(const JSONRPCRequest& request);
extern UniValue importwallet(const JSONRPCRequest& request);
extern UniValue importprunedfunds(const JSONRPCRequest& request);
extern UniValue removeprunedfunds(const JSONRPCRequest& request);
extern UniValue importmulti(const JSONRPCRequest& request);
extern UniValue rescanblockchain(const JSONRPCRequest& request);

static const CRPCCommand commands[] =
{ //  category              name                        actor (function)           argNames
    //  --------------------- ------------------------    -----------------------  ----------
    { "rawtransactions",    "fundrawtransaction",       &fundrawtransaction,       {"hexstring","options","iswitness"} },
    { "hidden",             "resendwallettransactions", &resendwallettransactions, {} },
    { "wallet",             "abandontransaction",       &abandontransaction,       {"txid"} },
    { "wallet",             "abortrescan",              &abortrescan,              {} },
    { "wallet",             "addmultisigaddress",       &addmultisigaddress,       {"nrequired","keys","account","address_type"} },
    { "hidden",             "addwitnessaddress",        &addwitnessaddress,        {"address","p2sh"} },
    { "wallet",             "backupwallet",             &backupwallet,             {"destination"} },
    { "wallet",             "bumpfee",                  &bumpfee,                  {"txid", "options"} },
    { "wallet",             "dumpprivkey",              &dumpprivkey,              {"address"}  },
    { "wallet",             "dumpwalletprivatekeys",    &dumpwalletprivatekeys,    {"filename"} },
    { "wallet",             "encryptwallet",            &encryptwallet,            {"passphrase"} },
    { "wallet",             "getaccountaddress",        &getaccountaddress,        {"account"} },
    { "wallet",             "getaccount",               &getaccount,               {"address"} },
    { "wallet",             "getaddressesbyaccount",    &getaddressesbyaccount,    {"account"} },
    { "wallet",             "getbalance",               &getbalance,               {"account","minconf","include_watchonly"} },
    { "wallet",             "getnewaddress",            &getnewaddress,            {"account","address_type"} },
    { "wallet",             "getrawchangeaddress",      &getrawchangeaddress,      {"address_type"} },
    { "wallet",             "getreceivedbyaccount",     &getreceivedbyaccount,     {"account","minconf"} },
    { "wallet",             "getreceivedbyaddress",     &getreceivedbyaddress,     {"address","minconf"} },
    { "wallet",             "gettransaction",           &gettransaction,           {"txid","include_watchonly"} },
    { "wallet",             "getunconfirmedbalance",    &getunconfirmedbalance,    {} },
    { "wallet",             "getwalletinfo",            &getwalletinfo,            {} },
    { "wallet",             "importmulti",              &importmulti,              {"requests","options"} },
    { "wallet",             "importprivkey",            &importprivkey,            {"privkey","label","rescan"} },
    { "wallet",             "importwallet",             &importwallet,             {"filename"} },
    { "wallet",             "importaddress",            &importaddress,            {"address","label","rescan","p2sh"} },
    { "wallet",             "importprunedfunds",        &importprunedfunds,        {"rawtransaction","txoutproof"} },
    { "wallet",             "importpubkey",             &importpubkey,             {"pubkey","label","rescan"} },
    { "wallet",             "keypoolrefill",            &keypoolrefill,            {"newsize"} },
    { "wallet",             "listaccounts",             &listaccounts,             {"minconf","include_watchonly"} },
    { "wallet",             "listaddressgroupings",     &listaddressgroupings,     {} },
    { "wallet",             "listlockunspent",          &listlockunspent,          {} },
    { "wallet",             "listreceivedbyaccount",    &listreceivedbyaccount,    {"minconf","include_empty","include_watchonly"} },
    { "wallet",             "listreceivedbyaddress",    &listreceivedbyaddress,    {"minconf","include_empty","include_watchonly"} },
    { "wallet",             "listsinceblock",           &listsinceblock,           {"blockhash","target_confirmations","include_watchonly","include_removed"} },
    { "wallet",             "listtransactions",         &listtransactions,         {"account","count","skip","include_watchonly"} },
    { "wallet",             "listunspent",              &listunspent,              {"minconf","maxconf","addresses","include_unsafe","query_options"} },
    { "wallet",             "listwallets",              &listwallets,              {} },
    { "wallet",             "lockunspent",              &lockunspent,              {"unlock","transactions"} },
    { "wallet",             "move",                     &movecmd,                  {"fromaccount","toaccount","amount","minconf","comment"} },
    { "wallet",             "sendfrom",                 &sendfrom,                 {"fromaccount","toaddress","amount","minconf","comment","comment_to"} },
    { "wallet",             "sendmany",                 &sendmany,                 {"fromaccount","amounts","minconf","comment","subtractfeefrom","replaceable","conf_target","estimate_mode"} },
    { "wallet",             "sendtoaddress",            &sendtoaddress,            {"address","amount","comment","comment_to","subtractfeefromamount","replaceable","conf_target","estimate_mode"} },
    { "wallet",             "setaccount",               &setaccount,               {"address","account"} },
    { "wallet",             "settxfee",                 &settxfee,                 {"amount"} },
    { "wallet",             "signmessage",              &signmessage,              {"address","message"} },
    { "wallet",             "walletlock",               &walletlock,               {} },
    { "wallet",             "walletpassphrasechange",   &walletpassphrasechange,   {"oldpassphrase","newpassphrase"} },
    { "wallet",             "walletpassphrase",         &walletpassphrase,         {"passphrase","timeout"} },
    { "wallet",             "removeprunedfunds",        &removeprunedfunds,        {"txid"} },
    { "wallet",             "rescanblockchain",         &rescanblockchain,         {"start_height", "stop_height"} },
    { "wallet",             "getfeeforamount",          &getfeeforamount,          {"amount", "address"} },
    { "generating",         "generate",                 &generate,                 {"nblocks","maxtries"} },
    // NIX Staking functions
    { "wallet",             "getstakinginfo",           &getstakinginfo,           {} },
    { "wallet",             "getcoldstakinginfo",       &getcoldstakinginfo,       {} },
    { "wallet",             "reservebalance",           &reservebalance,           {"enabled","amount"} },
    { "wallet",             "getalladdresses",          &getalladdresses,          {} },
    { "wallet",             "manageaddressbook",        &manageaddressbook,        {"action","address","label","purpose"} },
    { "wallet",             "getstakingaverage",        &getstakingaverage,        {} },
    { "wallet",             "leasestaking",             &leasestaking,             {"lease address","amount", "fee percent","lease percent reward address", "comment","comment_to","subtractfeefromamount","replaceable","conf_target","estimate_mode"} },
    { "wallet",             "getleasestakinglist",      &getleasestakinglist,      {} },
    { "wallet",             "cancelstakingcontract",    &cancelstakingcontract,    {"tx_hash","tx_index", "amount"} },
    // NIX Ghost functions (experimental)
    { "NIX Privacy",        "listunspentghostednix",    &listunspentmintzerocoins, {} },
    { "NIX Privacy",        "ghostamount",              &ghostamount,              {"amount"} },
    { "NIX Privacy",        "unghostamount",            &unghostamount,            {"amount"} },
    { "NIX Privacy",        "resetghostednix",          &resetmintzerocoin,        {} },
    { "NIX Privacy",        "setghostednixstatus",      &setmintzerocoinstatus,    {} },
    { "NIX Privacy",        "listghostednix",           &listmintzerocoins,        {"all"} },
    { "NIX Privacy",        "listpubcoins",             &listpubcoins,             {} },
    { "NIX Privacy",        "refillghostkeys",          &refillghostkeys,          {"amount"} },
    { "NIX Privacy",        "listunloadedpubcoins",     &listunloadedpubcoins,     {"amount"} },
    { "NIX Privacy",        "payunloadedpubcoins",      &payunloadedpubcoins,      {"amount", "address"} },
    { "NIX Privacy",        "getpubcoinpack",           &getpubcoinpack,           {"amount"} },
    { "NIX Privacy",        "resetzerocoinamounts",     &resetzerocoinamounts,     {} },
    { "NIX Privacy",        "resetzerocoinunconfirmed", &resetzerocoinunconfirmed, {} },
    { "NIX Privacy",        "listallserials",           &listallserials,           {"height"} },
    { "NIX Privacy",        "eraseusedzerocoindata",    &eraseusedzerocoindata,    {""} },
    { "NIX Privacy",        "encryptallzerocoins",      &encryptallzerocoins,      {""} },
    { "NIX Privacy",        "decryptallzerocoins",      &decryptallzerocoins,      {""} },
    { "NIX Privacy",        "ghostfeepayouttotal",      &ghostfeepayouttotal,      {""} },
    { "NIX Privacy",        "ghostprivacysets",         &ghostprivacysets,         {""} },
    { "NIX Privacy",        "mintghostdata",            &mintghostdata,            {""} },
    { "NIX Privacy",        "spendghostdata",           &spendghostdata,           {""} },
    // NIX Lite Zerocoin
    { "NIX Privacy",        "getzerocoinacc",           &getzerocoinacc,           {""} },
    { "NIX Privacy",        "getdatazerocoinacc",       &getdatazerocoinacc,       {""} },
    // Sigma functions
    { "NIX Privacy",        "getpubcoinpackv2",         &getpubcoinpackv2,         {"amount"} },
    { "NIX Privacy",        "ghostamountv2",            &ghostamountv2,            {"amount", "commitment_key_pack"} },
    { "NIX Privacy",        "unghostamountv2",          &unghostamountv2,          {"amount", "to_key"} },
    { "NIX Privacy",        "getsigmaseed",             &getsigmaseed,             {} },
    { "NIX Privacy",        "setsigmaseed",             &setsigmaseed,             {"seed"} },
    { "NIX Privacy",        "listsigmaentries",         &listsigmaentries,         {"all"} },
    { "NIX Privacy",        "ghostprivacysetsv2",       &ghostprivacysetsv2,       {""} },
    //NIX TOR routing functions
    { "NIX Privacy",        "enabletor",                &enableTor,                {"set"} },
    { "NIX Privacy",        "torstatus",                &torStatus,                {} },
  //NIX Governance functions
    { "NIX Governance",     "getoffchainproposals",     &getoffchainproposals,     {} },
    { "NIX Governance",     "postoffchainproposals",    &postoffchainproposals,    {"vote_id", "decision"} },
    { "NIX Governance",     "getvoteweight",            &getvoteweight,            {"start_time", "end_time"} },
    { "NIX Governance",     "erasegoventries",          &erasegoventries,          {""} },
};

void RegisterWalletRPCCommands(CRPCTable &t)
{
    for (unsigned int vcidx = 0; vcidx < ARRAYLEN(commands); vcidx++)
        t.appendCommand(commands[vcidx].name, &commands[vcidx]);
}
