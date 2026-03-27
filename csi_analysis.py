import pandas as pd
import numpy as np
import matplotlib.pyplot as plt

# =========================
# 1. LOAD DATA
# =========================
def load_file(path):
    if path.endswith(".xlsx"):
        return pd.read_excel(path)
    else:
        return pd.read_csv(path)

near = load_file("near.xlsx")
far  = load_file("far.xlsx")


# =========================
# 2. CLEANING
# =========================
def clean_csi(df, label):
    print(f"\n==== Cleaning {label} ====")
    
    original_size = len(df)

    df = df.dropna()
    df = df[(df["Pi5_AP1_amp"] > 0) & (df["Pi5_AP2_amp"] > 0)]
    df = df.drop_duplicates()

    print("Original rows:", original_size)
    print("After cleaning:", len(df))
    
    return df

near_clean = clean_csi(near, "NEAR")
far_clean  = clean_csi(far, "FAR")


# =========================
# 3. FILTER VALID SUBCARRIERS
# =========================
# Remove DC + edge + distorted carriers
#valid_subcarriers = list(range(2, 27))   # CORE BAND

valid_subcarriers = list(range(0, 63))   # Arya Checking for the entire range
near_clean = near_clean[near_clean["Subcarrier"].isin(valid_subcarriers)]
far_clean  = far_clean[far_clean["Subcarrier"].isin(valid_subcarriers)]

print("\nUsing subcarriers:", valid_subcarriers)


# =========================
# 4. PER-CARRIER ANALYSIS
# =========================
def per_carrier_analysis(near, far, ap):
    print(f"\n======================================")
    print(f" PER-CARRIER ANALYSIS ({ap})")
    print(f"======================================")

    print(f"{'SC':>5} | {'Near':>8} {'Far':>8} | {'Diff':>8} | {'Corr':>6} | Status")
    print("-"*65)

    good = 0
    total = 0

    corrs = []

    for sc in sorted(near["Subcarrier"].unique()):
        n = near[near["Subcarrier"] == sc][f"{ap}_amp"]
        f = far[far["Subcarrier"] == sc][f"{ap}_amp"]

        if len(n) < 20 or len(f) < 20:
            continue

        n_mean = n.mean()
        f_mean = f.mean()
        diff = n_mean - f_mean

        min_len = min(len(n), len(f))
        corr = np.corrcoef(n[:min_len], f[:min_len])[0,1]

        corrs.append(corr)

        # VALIDATION RULE
        if diff > 2 and corr < 0.7:
            status = "GOOD"
            good += 1
        else:
            status = "BAD"

        total += 1

        print(f"{sc:5d} | {n_mean:8.2f} {f_mean:8.2f} | {diff:8.2f} | {corr:6.3f} | {status}")

    print("\n------------------------------")
    print(f"Good carriers: {good}/{total}")
    print(f"Avg correlation: {np.mean(corrs):.3f}")

    if good > 0.7 * total:
        print("✅ STRONG CSI (VALID)")
    elif good > 0.4 * total:
        print("⚠️ PARTIAL CSI")
    else:
        print("❌ WEAK CSI (INVALID)")

    return corrs


# Run per-carrier analysis
corr1 = per_carrier_analysis(near_clean, far_clean, "Pi5_AP1")
corr2 = per_carrier_analysis(near_clean, far_clean, "Pi5_AP2")


# =========================
# 5. PER-CARRIER PLOT
# =========================
def plot_per_carrier(near, far):
    plt.figure(figsize=(12,6))

    near_ap1 = near.groupby("Subcarrier")["Pi5_AP1_amp"].mean()
    far_ap1  = far.groupby("Subcarrier")["Pi5_AP1_amp"].mean()

    near_ap2 = near.groupby("Subcarrier")["Pi5_AP2_amp"].mean()
    far_ap2  = far.groupby("Subcarrier")["Pi5_AP2_amp"].mean()

    plt.plot(near_ap1, label="AP1 Near")
    plt.plot(far_ap1,  label="AP1 Far")

    plt.plot(near_ap2, linestyle="--", label="AP2 Near")
    plt.plot(far_ap2,  linestyle="--", label="AP2 Far")

    plt.title("Per-Carrier Comparison (Filtered)")
    plt.xlabel("Subcarrier")
    plt.ylabel("Amplitude")
    plt.legend()
    plt.grid()

    plt.show()

plot_per_carrier(near_clean, far_clean)